"""Live tests: run real headless Claude Code sessions with a tiny compaction window and check that
the hook holds compaction, gets a handoff written, and loads it afterwards.

Run: make live, or python3 tests/live.py [scenario ...]
(scenarios: main stop parallel project cycles)
Costs $0.25-0.80 per scenario on Sonnet, about $3 for all five. Re-run after Claude Code upgrades: the behaviour
these hooks rely on is mostly undocumented.
"""
import glob
import json
import os
import random
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
HOOK = os.path.abspath(os.path.join(HERE, "..", "build", "auto-handoff"))
# Each run gets a fresh folder here, next to the real one; nothing is deleted.
HANDOFFS = os.path.expanduser("~/.local/state/claude-handoffs-livetest")
EVENTS = ["SessionStart", "SubagentStart", "SubagentStop", "UserPromptSubmit", "PostToolBatch", "Stop", "PreCompact"]
FOLLOW = "Follow any [auto-handoff] instructions you receive."

# Every scenario compacts at about 67k tokens: a 100k window (the smallest Claude Code accepts) set for
# the model in settings, as /autocompact sets it. Each file is about 12.9k tokens.
SCENARIOS = {
    # Compaction is held mid-turn, the agent is reminded, writes its handoff, and continues from it.
    "main": dict(turns=[
        "Read f01.txt through f08.txt with the Read tool, in order, strictly one file per message. " + FOLLOW +
        " When done, reply with the Owner line of the handoff you continued from, if any."]),
    # Compaction first waits on the final reply, so the Stop hook has to ask for the handoff.
    # (Crossed after the fifth file, before the reply, but not after the fourth.)
    "stop": dict(turns=[
        "Read f01.txt, f02.txt, f03.txt, f04.txt and f05.txt with the Read tool, strictly one file per message, "
        "then reply with just: done. " + FOLLOW]),
    # Two subagents compact at about the same time and each continues from its own handoff.
    "parallel": dict(turns=[
        "In ONE message, make TWO Agent tool calls at the same time (parallel, foreground, general-purpose), "
        "then relay both final reports verbatim. Agent 1 task text: 'Read f01.txt, f02.txt, f03.txt, f04.txt, "
        "f05.txt, f06.txt, f07.txt with the Read tool, strictly one file per message. " + FOLLOW +
        " Finally report the Owner line of the handoff you continued from, if any.' Agent 2 task text: 'Read "
        "f12.txt, f11.txt, f10.txt, f09.txt, f08.txt, f07.txt, f06.txt with the Read tool, strictly one file per "
        "message. " + FOLLOW + " Finally report the Owner line of the handoff you continued from, if any.'"]),
    # The session already keeps docs/HANDOFF.md: it's updated in place and the slot points at it.
    "project": dict(project_handoff=True, turns=[
        "This project keeps its session handoff in docs/HANDOFF.md; whenever a handoff is needed, update that "
        "file and make sure it contains the line 'Marker: PLUM-42'. Now read f01.txt through f07.txt with the "
        "Read tool, in order, strictly one file per message. " + FOLLOW + " When done, reply with the Marker "
        "line of the handoff you continued from, if any."]),
    # Two compactions in one session: the second handoff is a rewrite, not the first one plus more.
    "cycles": dict(turns=[
        "Read f01.txt through f11.txt with the Read tool, in order, strictly one file per message. " + FOLLOW +
        " When done, reply with just: done."]),
}


def fillers(work):
    words = [w.strip() for w in open("/usr/share/dict/words") if w.strip().isalpha() and len(w.strip()) < 9]
    rnd = random.Random(1)
    for i in range(1, 13):  # about 12.9k tokens each
        with open(os.path.join(work, "f%02d.txt" % i), "w") as f:
            for _ in range(300):
                f.write(" ".join(rnd.choice(words) for _ in range(12)) + "\n")


def run(name, spec, tmp, run_dir):
    work = os.path.join(tmp, name)
    os.makedirs(work)
    fillers(work)
    if spec.get("project_handoff"):
        os.makedirs(os.path.join(work, "docs"))
        with open(os.path.join(work, "docs", "HANDOFF.md"), "w") as f:
            f.write("# Session handoff\n\nNothing yet.\n")
    handoffs = os.path.join(run_dir, name)
    log = os.path.join(tmp, name + ".log")
    settings = {
        "hooks": {e: [{"hooks": [{"type": "command", "command": HOOK, "timeout": 10}]}] for e in EVENTS},
        "permissions": {"allow": ["%s(~/.local/state/claude-handoffs-livetest/**)" % t for t in ("Read", "Edit")]},
        "modelSettings": {"claude-sonnet-5-5": {"autoCompactWindow": 100000}},
    }
    with open(os.path.join(tmp, name + ".settings.json"), "w") as f:
        json.dump(settings, f)
    env = dict(os.environ, AUTO_HANDOFF_DIR=handoffs, AUTO_HANDOFF_LOG=log,
               AUTO_HANDOFF_SETTINGS=os.path.join(tmp, name + ".settings.json"))
    cmd = ["claude", "-p", "--model", "sonnet", "--strict-mcp-config", "--setting-sources", "project",
           "--settings", os.path.join(tmp, name + ".settings.json"), "--input-format", "stream-json",
           "--output-format", "stream-json", "--verbose", "--allowedTools", "Read", "Agent", "Task"]
    if spec.get("project_handoff"):
        cmd += ["Edit(docs/**)"]  # a real session edits its project files
    p = subprocess.Popen(cmd, cwd=work, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    out = {"results": [], "boundaries": 0, "cost": 0.0, "session": None, "denials": []}
    for turn in spec["turns"]:
        p.stdin.write(json.dumps({"type": "user", "message": {"role": "user", "content": turn}}) + "\n")
        p.stdin.flush()
        for line in p.stdout:
            o = json.loads(line)
            if o.get("type") == "system" and o.get("subtype") == "compact_boundary":
                out["boundaries"] += 1
            if o.get("type") == "result":
                out["results"].append(o.get("result") or "")
                out["cost"] += o.get("total_cost_usd") or 0
                out["session"] = o.get("session_id")
                out["denials"] += [d.get("tool_name") for d in o.get("permission_denials") or []]
                break
    p.stdin.close()
    p.wait(timeout=120)
    out["log"] = open(log).read() if os.path.exists(log) else ""
    out["work"] = work
    out["archive"] = glob.glob(os.path.join(handoffs, "archive", "*", "*.md"))
    out["handoffs"] = {os.path.basename(f): open(f).read() for f in glob.glob(os.path.join(handoffs, "*", "*.md"))
                       if not f.endswith(".facts.md")}
    out["facts"] = {os.path.basename(f): open(f).read() for f in glob.glob(os.path.join(handoffs, "*", "*.facts.md"))}
    tdir = glob.glob(os.path.expanduser("~/.claude/projects/*/%s" % out["session"]))
    out["sub_transcripts"] = glob.glob(os.path.join(tdir[0], "subagents", "agent-*.jsonl")) if tdir else []
    main_t = glob.glob(os.path.expanduser("~/.claude/projects/*/%s.jsonl" % out["session"]))
    out["main_transcript"] = open(main_t[0]).read() if main_t else ""
    return out


def summary_lengths(text):
    """Characters in each compaction summary in a transcript."""
    sizes = []
    for line in text.splitlines():
        if '"isCompactSummary":true' not in line.replace(" ", ""):
            continue
        content = json.loads(line).get("message", {}).get("content")
        sizes.append(len(content if isinstance(content, str) else json.dumps(content)))
    return sizes


def writes_to(text, name):
    """What a transcript's Write calls put in files named `name`, in order. Slots are removed once
    their handoff is loaded, so this is where the written handoffs are seen afterwards."""
    found = []
    for line in text.splitlines():
        if '"tool_use"' not in line or name not in line:
            continue
        for b in json.loads(line).get("message", {}).get("content", []):
            if isinstance(b, dict) and b.get("type") == "tool_use" and b["input"].get("file_path", "").endswith("/" + name):
                found.append(b["input"].get("content", ""))
    return found


def check(name, out):
    log, ok = out["log"], []

    def expect(cond, what):
        ok.append(bool(cond))
        print("  %s %s" % ("PASS" if cond else "FAIL", what))

    expect(not out["denials"], "no permission denials (got %s)" % out["denials"])
    expect("released but not compacted" not in log, "every release reached the agent it was meant for")
    if name in ("main", "stop"):
        expect("compaction of main held" in log, "compaction held for main")
        expect(writes_to(out["main_transcript"], "main.md"), "main handoff written")
        expect("compaction of main allowed: handoff" in log, "released by the handoff")
        expect("compacted: 1 handoff(s) to load" in log, "handoff loaded after compaction")
        expect(out["boundaries"] >= 1, "compacted (%d)" % out["boundaries"])
        expect("just compacted" in out["main_transcript"], "injection reached the main transcript")
        sizes = summary_lengths(out["main_transcript"])
        expect(sizes and max(sizes) < 2000, "short summary (%s chars; about 2,200-3,600 by default)" % sizes)
    if name == "main":
        expect("PostToolBatch nudge for main" in log, "nudged mid-turn")
        facts = out["facts"].get("main.facts.md", "")
        expect("Read f01.txt through f08.txt" in facts, "facts file holds the user's request word for word")
        expect("main.facts.md" in out["main_transcript"], "facts file named after compaction")
    if name in ("main", "stop"):
        expect(len(out["archive"]) == 1, "handoff archived (%d)" % len(out["archive"]))
    if name == "project":
        project = os.path.join(os.path.realpath(out["work"]), "docs", "HANDOFF.md")
        slot = (writes_to(out["main_transcript"], "main.md") or [""])[-1].strip()
        expect(os.path.realpath(slot) == project if slot.startswith("/") else False, "slot points at docs/HANDOFF.md (%r)" % slot[:120])
        expect("PLUM-42" in open(project).read(), "docs/HANDOFF.md updated in place")
        expect("compaction of main allowed: handoff" in log, "released by the slot")
        injected = [l for l in out["main_transcript"].splitlines() if '"hook_additional_context"' in l and "just compacted" in l]
        expect(injected and "PLUM-42" in injected[-1], "project handoff loaded after compaction")
        expect(len(out["archive"]) == 1, "archived (%d)" % len(out["archive"]))
        sizes = summary_lengths(out["main_transcript"])
        expect(sizes and max(sizes) < 2000, "short summary (%s chars)" % sizes)
    if name == "cycles":
        writes = writes_to(out["main_transcript"], "main.md")
        expect(log.count("compacted: 1 handoff(s) to load") >= 2, "compacted twice with a handoff")
        sizes = summary_lengths(out["main_transcript"])
        expect(len(sizes) >= 2 and max(sizes) < 2000, "short summaries, the second one too (%s chars)" % sizes)
        expect(len(writes) >= 2, "%d handoff writes" % len(writes))
        if len(writes) >= 2:
            first_body = "\n".join(writes[0].splitlines()[1:]).strip()
            expect(first_body[:400] not in writes[-1], "second handoff is a rewrite, not the first plus more")
            print("  handoff sizes: %s" % [len(w) for w in writes])
    if name == "stop":
        expect("Stop held for main's handoff" in log, "Stop hook asked for the handoff")
    if name == "parallel":
        subs = {}
        for t in out["sub_transcripts"]:
            agent = os.path.basename(t)[len("agent-"):-len(".jsonl")]
            written = writes_to(open(t).read(), agent + ".md")
            if written:
                subs[agent] = written[-1]
        expect(len(subs) == 2, "2 subagent handoffs written: %s" % sorted(subs))
        for agent, handoff in subs.items():
            expect("compaction of %s allowed: handoff" % agent in log, "%s released by its handoff" % agent[:8])
            t = [x for x in out["sub_transcripts"] if x.endswith("agent-%s.jsonl" % agent)]
            text = open(t[0]).read() if t else ""
            own = handoff.strip().splitlines()[0]
            expect("just compacted" in text and json.dumps(own)[1:-1] in text, "%s got its own handoff back" % agent[:8])
            sizes = summary_lengths(text)
            expect(sizes and max(sizes) < 2000, "%s short summary (%s chars)" % (agent[:8], sizes))
        expect("compaction of main" not in log, "main was never mistaken for the compacting agent")
    return all(ok)


def main():
    names = sys.argv[1:] or list(SCENARIOS)
    tmp = tempfile.mkdtemp(prefix="auto-handoff-live-")
    os.makedirs(HANDOFFS, exist_ok=True)
    run_dir = tempfile.mkdtemp(dir=HANDOFFS)
    outs = {}
    threads = [threading.Thread(target=lambda n=n: outs.__setitem__(n, run(n, SCENARIOS[n], tmp, run_dir)))
               for n in names]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    passed = True
    for n in names:
        out = outs.get(n)
        print("== %s ($%.2f)" % (n, out["cost"] if out else 0))
        if not out:
            print("  FAIL scenario crashed")
            passed = False
            continue
        print("  log:\n    " + out["log"].strip().replace("\n", "\n    "))
        for r in out["results"]:
            print("  result: " + r[:600].replace("\n", " | "))
        passed &= check(n, out)
    print("\nall passed" if passed else "\nSOME CHECKS FAILED", "| work files in", tmp, "| handoffs in", run_dir)
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
