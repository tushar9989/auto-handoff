"""Unit tests for the auto-handoff hook: feed it hook events the way Claude Code does and check what
it says back. Run: make test (or AUTO_HANDOFF_BIN=path python3 tests/test_hook.py)"""
import json
import os
import subprocess
import tempfile
import time
import unittest

HOOK = os.environ.get("AUTO_HANDOFF_BIN") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "auto-handoff")
SID = "11111111-2222-3333-4444-555555555555"
NOTE = "A note from the user: make no mistakes. I love you."


class HookTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, "handoffs")
        self.config = os.path.join(self.tmp.name, "config")
        self.proj = os.path.join(self.tmp.name, "proj")
        os.makedirs(os.path.join(self.proj, SID, "subagents"))
        os.makedirs(self.config)
        with open(os.path.join(self.config, "settings.json"), "w") as f:
            json.dump({"autoCompactWindow": 333000}, f)
        self.transcript = os.path.join(self.proj, SID + ".jsonl")
        self.d = os.path.join(self.root, SID)
        self.tokens("main", 300_000)

    def tearDown(self):
        self.tmp.cleanup()

    # ---- helpers

    def env(self):
        # settings.json sets a 333k window, as installed, so compaction first tries at about 300k.
        return dict(os.environ, AUTO_HANDOFF_DIR=self.root, CLAUDE_CONFIG_DIR=self.config)

    def transcript_path(self, agent):
        if agent == "main":
            return self.transcript
        return os.path.join(self.proj, SID, "subagents", "agent-%s.jsonl" % agent)

    def tokens(self, agent, n, model="claude-opus-5-5", path=None):
        """Make an agent's transcript end with a request of n context tokens."""
        p = path or self.transcript_path(agent)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        entry = {"type": "assistant", "message": {"model": model, "usage": {
            "input_tokens": 2, "cache_creation_input_tokens": 1000, "cache_read_input_tokens": n - 1002}}}
        with open(p, "w") as f:
            f.write(json.dumps({"type": "user", "message": {"content": "hi"}}) + "\n" + json.dumps(entry) + "\n")

    def append(self, agent, entry):
        with open(self.transcript_path(agent), "a") as f:
            f.write(json.dumps(entry) + "\n")

    def run_raw(self, inp, timeout=10):
        out = subprocess.run([HOOK], input=inp, capture_output=True, text=True, env=self.env(), timeout=timeout)
        self.assertEqual(out.returncode, 0)
        self.assertFalse(os.path.exists(os.path.join(self.root, "errors.log")))
        if not out.stdout.strip():
            return None
        try:
            return json.loads(out.stdout)
        except ValueError:
            return {"text": out.stdout}

    def run_hook(self, event, agent=None, sid=SID, **fields):
        inp = {"session_id": sid, "transcript_path": self.transcript, "cwd": "/work", "hook_event_name": event}
        if agent:
            inp.update(agent_id=agent, agent_type=fields.pop("agent_type", "general-purpose"))
        inp.update(fields)
        return self.run_raw(json.dumps(inp))

    def ctx(self, out):
        return (out or {}).get("hookSpecificOutput", {}).get("additionalContext", "")

    def tool(self, agent=None, response="ok"):
        """A batch of tool calls just finished: the agent's next request follows."""
        return self.run_hook("PostToolBatch", agent, tool_calls=[
            {"tool_name": "Read", "tool_input": {"file_path": "x"}, "tool_use_id": "toolu_1", "tool_response": response}])

    def compact(self, trigger="auto"):
        return self.run_hook("PreCompact", trigger=trigger, custom_instructions=None)

    def returned(self, agent="main"):
        """Compaction ran in this agent: Claude Code marks its transcript, then loads what the hook says."""
        self.append(agent, {"type": "system", "subtype": "compact_boundary",
                            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime())})
        return self.ctx(self.run_hook("SessionStart", source="compact"))

    def blocked(self, out):
        return (out or {}).get("decision") == "block"

    def shortens_summary(self, out):
        return "keep the summary under 100 words" in (out or {}).get("text", "")

    def write_handoff(self, agent, text):
        os.makedirs(self.d, exist_ok=True)
        with open(os.path.join(self.d, agent + ".md"), "w") as f:
            f.write(text)

    def agents(self):
        p = os.path.join(self.d, "state.json")
        if not os.path.exists(p):
            return {}
        with open(p) as f:
            return json.load(f)["agents"]

    def age(self, agent, seconds, what="events"):
        """Make an agent's last activity (or its announcement, with what="crossing") look this old."""
        p = os.path.join(self.d, what, agent)
        os.utime(p, (time.time() - seconds, time.time() - seconds))

    def archived(self):
        d = os.path.join(self.root, "archive", "-work")
        return sorted(os.listdir(d)) if os.path.isdir(d) else []

    def project_handoff(self, text, git=False):
        """A HANDOFF.md the session already keeps in its project, optionally committed to git."""
        repo = os.path.join(self.tmp.name, "game")
        os.makedirs(os.path.join(repo, "docs"), exist_ok=True)
        p = os.path.join(repo, "docs", "HANDOFF.md")
        with open(p, "w") as f:
            f.write(text)
        if git:
            for cmd in (["init", "-q"], ["add", "docs/HANDOFF.md"],
                        ["-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "handoff"]):
                subprocess.run(["git", "-C", repo] + cmd, check=True, capture_output=True)
        return p

    def hold(self, agent=None):
        self.tool(agent)
        out = self.compact()
        self.assertTrue(self.blocked(out))
        return out

    # ---- the main flow

    def test_main_flow(self):
        self.assertIn("main.md", self.ctx(self.run_hook("SessionStart", source="startup")))
        self.hold()
        first = self.ctx(self.tool())
        self.assertIn("compaction is waiting", first)
        self.assertIn(os.path.join(self.d, "main.md"), first)
        self.assertIn("about 300k tokens", first)
        self.assertIn("Rewrite it rather than append", first)  # housekeeping rules
        self.assertIn("already keeps a handoff file", first)  # an existing handoff stays where it is
        quiet = [self.ctx(self.tool()) for _ in range(3)]
        self.assertEqual(quiet, ["", "", ""])
        self.assertIn("Reminder", self.ctx(self.tool()))  # every 5th tool call
        self.assertTrue(self.blocked(self.compact()))
        self.write_handoff("main", "Owner: main session - port the parser\nNext: run the tests")
        self.assertEqual(self.ctx(self.tool()), "")  # no reminder once it's written
        self.assertTrue(self.shortens_summary(self.compact()))
        injected = self.returned()
        self.assertTrue(injected.startswith(NOTE))
        self.assertIn("handoff for the main session", injected)
        self.assertIn("Next: run the tests", injected)
        self.tool()
        self.assertEqual(list(self.agents()["main"]), ["compacted"])
        self.assertFalse(os.path.exists(os.path.join(self.d, "main.md")))  # archived; the next one is a new file
        # The next cycle needs a new handoff: the old one is spent.
        self.tokens("main", 300_000)
        self.hold()
        self.assertIn("compaction is waiting", self.ctx(self.tool()))

    def settings(self, settings):
        with open(os.path.join(self.config, "settings.json"), "w") as f:
            json.dump(settings, f)

    def test_off_until_a_window_is_set(self):
        # Installed, but no window set (or the model's own): the hook says and holds nothing.
        for settings in ({}, {"modelSettings": {"claude-opus-5-5": {"autoCompactWindow": 1000000}}},
                         {"modelSettings": {"claude-opus-5-5": {"autoCompactWindow": "auto"}}}):
            self.tearDown()
            self.setUp()
            self.settings(settings)
            self.assertEqual(self.ctx(self.run_hook("SessionStart", source="startup")), "")
            self.assertEqual(self.ctx(self.run_hook("SubagentStart", "sub", agent_type="general-purpose")), "")
            self.tokens("main", 970_000)
            self.tool()
            self.assertFalse(self.blocked(self.compact()))
            self.assertEqual(self.returned(), "")  # no note, nothing loaded
            self.assertIsNone(self.compact("manual"))
            self.assertEqual(self.returned(), "")
            self.assertEqual(self.agents(), {})

    def test_on_once_a_window_is_set(self):
        self.settings({"modelSettings": {"claude-opus-5-5": {"autoCompactWindow": 333000}}})  # /autocompact 333k
        os.remove(self.transcript)  # a new session: no transcript yet, so the settings decide
        self.assertIn("main.md", self.ctx(self.run_hook("SessionStart", source="startup")))
        self.tokens("main", 300_000)
        self.hold()

    def test_switching_off_while_held_lets_the_agent_go(self):
        # /autocompact auto, or a window past about 890k: the hook is off, so main stops waiting at once,
        # even when still close to the new compaction point.
        for window in ("auto", 900000):
            self.tearDown()
            self.setUp()
            self.settings({"modelSettings": {"claude-opus-5-5": {"autoCompactWindow": 893000}}})
            self.tokens("main", 875_000)
            self.hold()
            self.settings({"modelSettings": {"claude-opus-5-5": {"autoCompactWindow": window}}})
            self.tokens("main", 876_000)
            self.assertEqual(self.ctx(self.tool()), "")
            self.assertIsNone(self.run_hook("Stop", stop_hook_active=False))
            self.assertEqual(list(self.agents()["main"]), ["compacted"])

    def test_handoff_written_before_waiting_is_stale(self):
        self.write_handoff("main", "Owner: main session - old")
        self.hold()
        self.assertTrue(self.blocked(self.compact()))

    def test_empty_slot_is_not_ready(self):
        self.hold()
        self.write_handoff("main", "  \n")
        self.tool()
        self.assertTrue(self.blocked(self.compact()))

    def test_handoff_ready_long_ago_is_released(self):
        # Written, then the turn ended: the compaction comes at the next request, however much later.
        self.hold()
        self.write_handoff("main", "Owner: main session - written, then interrupted")
        with open(os.path.join(self.d, "state.json")) as f:
            st = json.load(f)
        st["agents"]["main"]["pending"] = time.time() - 7200
        with open(os.path.join(self.d, "state.json"), "w") as f:
            json.dump(st, f)
        old = time.time() - 3600
        os.utime(os.path.join(self.d, "main.md"), (old, old))
        self.tool()
        self.assertTrue(self.shortens_summary(self.compact()))

    def test_outgrown_handoff_is_asked_for_again(self):
        # Asked early, main writes its handoff, then goes on working: by the time its compaction comes,
        # the handoff no longer covers the work since, so it's asked for again.
        self.hold()
        self.write_handoff("main", "Owner: main session - early")
        self.tool()  # the batch that wrote it
        self.tokens("main", 330_000)
        self.tool()
        self.assertTrue(self.blocked(self.compact()))
        self.assertIn("compaction is waiting", self.ctx(self.tool()))
        self.write_handoff("main", "Owner: main session - current")
        self.tool()
        self.assertTrue(self.shortens_summary(self.compact()))
        self.assertIn("current", self.returned())

    def test_shrunk_context_stops_waiting(self):
        # Rewound while a handoff was pending: no longer near the compaction point, so no more asking.
        self.hold()
        self.tokens("main", 200_000)
        self.assertEqual(self.ctx(self.tool()), "")
        self.assertNotIn("pending", self.agents()["main"])
        self.assertIsNone(self.run_hook("Stop", stop_hook_active=False))

    def test_stop_forces_handoff_once(self):
        self.hold()
        out = self.run_hook("Stop", stop_hook_active=False)
        self.assertTrue(self.blocked(out))
        self.assertIn("end of this turn is a clean point", out["reason"])
        self.assertIsNone(self.run_hook("Stop", stop_hook_active=True))  # never loop
        self.write_handoff("main", "Owner: main session - x")
        self.assertIsNone(self.run_hook("Stop", stop_hook_active=False))

    def test_user_prompt_gets_full_nudge(self):
        self.hold()
        self.assertIn("compaction is waiting", self.ctx(self.tool()))
        self.assertIn("compaction is waiting", self.ctx(self.run_hook("UserPromptSubmit", prompt="next")))

    def test_safety_limit(self):
        self.tokens("main", 460_000)
        self.tool()
        self.assertFalse(self.blocked(self.compact()))
        injected = self.returned()
        self.assertTrue(injected.startswith(NOTE))
        self.assertIn("before a handoff was written", injected)

    def test_unknown_model_uses_small_window(self):
        self.tokens("main", 185_000, model="some-new-model")
        self.tool()
        self.assertFalse(self.blocked(self.compact()))  # assumed to have a 200k window: off

    def test_fable_has_a_million_window(self):
        self.tokens("main", 300_000, model="claude-fable-5-1")
        self.hold()  # not mistaken for a 200k model and compacted at once

    def test_api_error_entries_dont_hide_the_size(self):
        self.append("main", {"type": "assistant", "isApiErrorMessage": True, "message": {
            "model": "<synthetic>", "usage": {"input_tokens": 0, "cache_read_input_tokens": 0}}})
        self.hold()
        self.assertEqual(self.agents()["main"]["tokens"] // 1000, 300)  # the size of the next request

    def test_manual_compact(self):
        self.tool()
        self.assertIsNone(self.compact("manual"))
        back = self.returned()
        self.assertTrue(back.startswith(NOTE))  # nothing to load, but the note still comes
        self.assertNotIn("just compacted", back)
        self.tool()
        self.write_handoff("main", "Owner: main session - manual one")
        self.assertTrue(self.shortens_summary(self.compact("manual")))
        self.assertIn("manual one", self.returned())

    def test_unannounced_compaction_near_the_point_is_held(self):
        # Say idle compaction, or a background agent's report reaching main: no hook announced it.
        self.tokens("main", 280_000)
        self.assertTrue(self.blocked(self.compact()))
        self.assertIn("compaction is waiting", self.ctx(self.run_hook("UserPromptSubmit", prompt="back")))

    def test_nobody_near_the_trigger_is_not_held(self):
        self.tokens("main", 120_000)
        self.tool()
        self.assertFalse(self.blocked(self.compact()))
        self.assertNotIn("main", self.agents())

    # ---- which agent is compacting

    def test_subagent_gets_its_own_path(self):
        self.tokens("main", 50_000)
        self.tokens("agentA", 300_000, model="claude-sonnet-5-5")
        self.assertIn("agentA.md", self.ctx(self.run_hook("SubagentStart", "agentA")))
        self.hold("agentA")
        self.assertNotIn("pending", self.agents().get("main", {}))
        self.assertEqual(self.ctx(self.tool()), "")  # main isn't nudged
        nudge = self.ctx(self.tool("agentA"))
        self.assertIn(os.path.join(self.d, "agentA.md"), nudge)
        self.assertIn("the text itself rather than a path", nudge)
        self.assertIn("belongs in your final report", nudge)  # not the project's docs or memory
        self.assertNotIn("already keeps a handoff file", nudge)
        self.write_handoff("agentA", "Owner: subagent - survey files")
        self.tool("agentA")
        self.assertTrue(self.shortens_summary(self.compact()))
        self.assertIn("survey files", self.returned())
        self.run_hook("SubagentStop", "agentA")
        self.assertNotIn("agentA", self.agents())

    def test_small_background_subagent_is_not_blamed(self):
        self.tokens("main", 310_000)
        self.tokens("agentB", 80_000)
        self.tool()
        self.tool("agentB")  # its tool call lands last
        self.assertTrue(self.blocked(self.compact()))
        self.assertIn("pending", self.agents()["main"])
        self.assertNotIn("agentB", self.agents())

    def test_new_subagent_without_transcript_is_not_blamed(self):
        self.tool()
        self.run_hook("SubagentStart", "agentNew")
        self.assertTrue(self.blocked(self.compact()))
        self.assertIn("pending", self.agents()["main"])
        self.assertNotIn("agentNew", self.agents())

    def test_subagent_in_plan_mode_is_not_held(self):
        self.tokens("main", 50_000)
        self.tokens("p", 300_000)
        self.run_hook("SubagentStart", "p", agent_type="general-purpose")
        self.run_hook("PostToolBatch", "p", permission_mode="plan", tool_calls=[
            {"tool_name": "Read", "tool_input": {"file_path": "x"}, "tool_use_id": "t", "tool_response": "ok"}])
        self.assertFalse(self.blocked(self.compact()))

    def test_tool_input_counts_toward_the_next_request(self):
        # Claude Code counts the tool calls of the latest response too, so a big Write moves the size.
        self.tokens("main", 50_000)
        self.tokens("agentA", 280_000)
        self.run_hook("PostToolBatch", "agentA", tool_calls=[
            {"tool_name": "Write", "tool_input": {"file_path": "big.md", "content": "w" * 60_000},
             "tool_use_id": "t", "tool_response": "File created"}])
        self.assertTrue(self.blocked(self.compact()))

    def test_old_announcement_is_ignored(self):
        # An agent that announced but didn't compact (it finished, or the estimate was off) is no
        # longer a candidate a few seconds later.
        self.tokens("main", 40_000)
        self.tokens("agentA", 300_000)
        self.tool("agentA")
        self.age("agentA", 30, what="crossing")
        self.assertFalse(self.blocked(self.compact()))
        self.assertNotIn("agentA", self.agents())

    def test_batch_results_count_toward_the_next_request(self):
        self.tokens("main", 50_000)
        self.tokens("agentA", 280_000)  # 17k below its 297k announcing point
        self.tool("agentA", response="x" * 30_000)  # 10k tokens: still below
        self.assertFalse(self.blocked(self.compact()))
        self.tool("agentA", response=[{"type": "image", "source": {"data": "A" * 300_000}}])  # an image is 2k
        self.assertFalse(self.blocked(self.compact()))
        self.tool("agentA", response=[{"type": "text", "text": "x" * 60_000}])  # 20k tokens: crossing
        self.assertTrue(self.blocked(self.compact()))
        self.assertEqual(self.agents()["agentA"]["tokens"] // 1000, 300)  # the size of the next request

    def test_failed_tool_in_a_batch_still_announces(self):
        self.tokens("main", 50_000)
        self.tokens("agentA", 300_000)
        self.run_hook("PostToolBatch", "agentA", tool_calls=[
            {"tool_name": "Bash", "tool_input": {"command": "false"}, "tool_use_id": "t1", "tool_response": "Exit code 1"},
            {"tool_name": "Read", "tool_input": {"file_path": "y"}, "tool_use_id": "t2", "tool_response": "ok"}])
        self.assertTrue(self.blocked(self.compact()))
        self.assertIn("pending", self.agents()["agentA"])

    def test_prompt_counts_toward_the_next_request(self):
        self.tokens("main", 280_000)
        self.run_hook("UserPromptSubmit", prompt="here is a big paste:\n" + "p" * 60_000)
        self.assertTrue(self.blocked(self.compact()))

    def test_workflow_agent_transcript_is_found(self):
        self.tokens("main", 50_000)
        p = os.path.join(self.proj, SID, "subagents", "workflows", "wf_abc", "agent-wfagent.jsonl")
        self.tokens("wfagent", 300_000, path=p)
        self.hold("wfagent")
        self.assertIn("pending", self.agents()["wfagent"])
        with open(os.path.join(self.d, "paths", "wfagent")) as f:
            self.assertEqual(f.read(), p)

    def test_compacted_elsewhere_settles(self):
        # main waits, but its compaction was released under another agent's name: once its own
        # transcript shows the compaction, it's no longer waiting and Stop isn't held.
        self.hold()
        self.append("main", {"type": "system", "subtype": "compact_boundary",
                             "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime(time.time() + 1))})
        self.returned()
        self.tool()
        self.assertNotIn("pending", self.agents()["main"])
        self.assertIsNone(self.run_hook("Stop", stop_hook_active=False))

    def test_parallel_agents_choose_their_own(self):
        self.tokens("main", 50_000)
        for agent in ("agentA", "agentB"):
            self.tokens(agent, 300_000)
            self.hold(agent)
            self.write_handoff(agent, "Owner: subagent - task of %s" % agent)
            self.tool(agent)
            self.assertTrue(self.shortens_summary(self.compact()))
        injected = self.returned()
        self.assertIn("YOUR handoff", injected)
        self.assertIn("task of agentA", injected)
        self.assertIn("task of agentB", injected)

    def test_parallel_handoffs_are_labelled_by_owner(self):
        self.tokens("agentA", 300_000)
        self.run_hook("SubagentStart", "agentA", agent_type="pixel-builder")
        for agent in ("main", "agentA"):
            self.hold(None if agent == "main" else agent)
            self.write_handoff(agent, "Owner: %s" % agent)
            self.tool(None if agent == "main" else agent)
            self.compact()
        injected = self.returned()
        self.assertIn("main.md, for the main session", injected)
        self.assertIn("agentA.md, for a subagent (pixel-builder)", injected)

    def test_single_handoff_says_whose_it_is(self):
        self.tokens("main", 50_000)
        self.tokens("agentA", 300_000)
        self.run_hook("SubagentStart", "agentA", agent_type="pixel-builder")
        self.hold("agentA")
        self.write_handoff("agentA", "Owner: subagent")
        self.tool("agentA")
        self.compact()
        self.assertIn("If that's you, continue from it; if it isn't, ignore it", self.returned())

    def test_wrong_pick_is_recovered(self):
        # A and B both wait with their handoffs ready. B's batch lands in the moment between A's
        # announcement and A's PreCompact, so B is released, but A is the one compacted.
        self.tokens("main", 50_000)
        for agent in ("A", "B"):
            self.tokens(agent, 300_000)
            self.run_hook("SubagentStart", agent, agent_type="builder")
            self.hold(agent)
            self.write_handoff(agent, "Owner: subagent - TASK %s" % agent)
        self.tool("A")
        time.sleep(0.01)
        self.tool("B")
        out = self.compact()
        self.assertIn(os.path.join(self.d, "B.md"), out["text"])  # B released: the summary instruction names B's slot
        back = self.returned("A")
        self.assertIn("TASK B", back)  # labelled as B's
        self.assertIn("ready but weren't loaded", back)
        self.assertIn(os.path.join(self.d, "A.md"), back)  # A finds its own
        self.tool("A")
        self.assertNotIn("pending", self.agents()["A"])
        self.assertTrue(os.path.exists(os.path.join(self.d, "A.md")))  # kept: it wasn't loaded
        self.tool("B")
        self.assertIn("pending", self.agents()["B"])  # not compacted: waiting again
        self.assertNotIn("released", self.agents()["B"])
        self.tool("B")
        self.assertTrue(self.shortens_summary(self.compact()))
        self.assertIn("TASK B", self.returned("B"))
        self.assertEqual(len([f for f in self.archived() if f.endswith("_A.md")]), 1)  # archived once
        self.assertEqual(len([f for f in self.archived() if f.endswith("_B.md")]), 1)

    def explore(self, agent, tokens):
        self.tokens(agent, tokens)
        self.run_hook("SubagentStart", agent, agent_type="Explore")

    def test_newest_announcement_is_the_one_compacting(self):
        self.tokens("main", 301_000)
        self.tokens("A", 300_000)
        self.tool()
        time.sleep(0.01)
        self.tool("A")
        self.assertTrue(self.blocked(self.compact()))
        self.assertIn("pending", self.agents()["A"])
        self.assertNotIn("main", self.agents())  # main announced too, but A's request came last

    def test_agent_below_its_point_is_not_a_candidate(self):
        self.tokens("main", 275_000)  # near its 300k point, but its next request doesn't cross it
        self.explore("exp", 301_000)
        self.tool()
        self.tool("exp")
        self.assertFalse(self.blocked(self.compact()))
        self.assertNotIn("main", self.agents())

    def test_alone_an_agent_that_cant_be_held_goes_through(self):
        self.tokens("main", 50_000)
        self.explore("exp", 280_000)
        self.tool("exp")
        self.assertFalse(self.blocked(self.compact()))

    def test_released_agent_is_let_through_again(self):
        self.tokens("main", 50_000)
        self.tokens("A", 305_000)
        self.hold("A")
        self.write_handoff("A", "Owner: subagent A")
        self.tool("A")
        self.assertTrue(self.shortens_summary(self.compact()))
        self.tool("A")  # the same compaction, tried again before it ran
        self.assertTrue(self.shortens_summary(self.compact()))

    def test_small_window_agent_near_its_trigger_is_never_blocked(self):
        self.tokens("main", 301_000)
        self.tokens("h", 165_000, model="claude-haiku-4-5")  # crosses its 167k trigger with main
        self.tool()
        self.tool("h")
        self.assertFalse(self.blocked(self.compact()))

    def test_small_window_agent_far_from_its_trigger_is_ignored(self):
        self.tokens("main", 301_000)
        self.tokens("h", 120_000, model="claude-haiku-4-5")
        self.tool()
        self.tool("h")
        self.assertTrue(self.blocked(self.compact()))
        self.assertIn("pending", self.agents()["main"])

    def test_agent_at_its_limit_goes_through(self):
        self.tokens("main", 300_000)
        self.tokens("A", 460_000)
        self.tool()
        time.sleep(0.01)
        self.tool("A")
        self.assertFalse(self.blocked(self.compact()))

    def test_unloaded_handoff_is_listed_after_compaction(self):
        self.hold()
        self.write_handoff("main", "Owner: main session - MINE")
        back = self.returned("main")  # compacted, but its handoff wasn't released
        self.assertIn("ready but weren't loaded", back)
        self.assertIn(os.path.join(self.d, "main.md"), back)
        self.tool()
        self.assertNotIn("pending", self.agents()["main"])
        self.assertEqual(len(self.archived()), 1)

    def test_usage_is_found_past_large_images(self):
        with open(self.transcript, "a") as f:
            for _ in range(8):  # image tool results after the last response, 650KB each
                f.write(json.dumps({"type": "user", "message": {"content": [{"type": "image", "data": "A" * 650_000}]}}) + "\n")
        self.hold()
        self.assertEqual(self.agents()["main"]["tokens"] // 1000, 300)  # the size of the next request

    def test_subagent_slot_never_points_at_a_file(self):
        handoff = self.project_handoff("# Shared handoff\n" + "z" * 20_000)
        self.tokens("main", 50_000)
        self.tokens("agentA", 300_000)
        self.hold("agentA")
        self.write_handoff("agentA", handoff)  # a path: taken as its handoff text, the file isn't touched
        self.tool("agentA")
        self.assertTrue(self.shortens_summary(self.compact()))
        injected = self.returned("agentA")
        self.assertIn(handoff, injected)
        self.assertNotIn("zzzz", injected)

    def test_agent_just_compacted_is_not_blamed(self):
        # Until its next request, a compacted agent's transcript still shows its old size.
        self.tokens("main", 50_000)
        self.tokens("A", 305_000)
        self.tokens("B", 302_000)
        self.hold("A")
        self.write_handoff("A", "Owner: subagent A")
        self.tool("A")
        self.compact()
        self.returned("A")
        self.tool("B")
        self.tool("A")  # A's activity is newest, but A is small now
        self.assertTrue(self.blocked(self.compact()))
        self.assertIn("pending", self.agents()["B"])

    def test_compaction_found_past_large_results(self):
        # The first batch after a compaction wrote over 4MB (screenshots): the compaction still counts.
        self.hold()
        self.write_handoff("main", "Owner: main session - first cycle")
        self.tool()
        self.assertTrue(self.shortens_summary(self.compact()))
        self.returned()
        with open(self.transcript, "a") as f:
            for _ in range(8):
                f.write(json.dumps({"type": "user", "message": {"content": [{"type": "image", "data": "A" * 650_000}]}}) + "\n")
            f.write(json.dumps({"type": "assistant", "message": {"model": "claude-opus-5-5", "usage": {
                "input_tokens": 1, "cache_read_input_tokens": 40_000}}}) + "\n")
        self.tool()
        self.assertEqual(list(self.agents()["main"]), ["compacted"])  # settled, not waiting again
        self.assertFalse(os.path.exists(os.path.join(self.d, "main.md")))

    def test_output_tokens_count(self):
        entry = {"type": "assistant", "message": {"model": "claude-opus-5-5", "usage": {
            "input_tokens": 1, "cache_read_input_tokens": 240_000, "output_tokens": 20_000}}}
        self.append("main", entry)
        self.hold()
        self.assertEqual(self.agents()["main"]["tokens"], 260_001)

    # ---- compaction points

    def test_small_window_model_is_not_held(self):
        # Claude Code refuses a 200k model's requests from 177k: no room to hold for a handoff safely,
        # even with a smaller window set for it.
        for settings in ({"autoCompactWindow": 333000},
                         {"modelSettings": {"claude-haiku-4-5": {"autoCompactWindow": 150000}}}):
            self.tearDown()
            self.setUp()
            self.settings(settings)
            self.tokens("main", 170_000, model="claude-haiku-4-5")
            self.assertEqual(self.ctx(self.run_hook("SessionStart", source="startup")), "")
            self.tool()
            self.assertFalse(self.blocked(self.compact()))
            self.assertNotIn("just compacted", self.returned())

    def test_window_settings(self):
        def held(tokens, model, settings):
            self.tearDown()
            self.setUp()
            self.tokens("main", tokens, model=model)
            with open(os.path.join(self.config, "settings.json"), "w") as f:
                json.dump(settings, f)
            self.tool()
            return self.blocked(self.compact())
        ms = lambda key, w: {"modelSettings": {key: {"autoCompactWindow": w}}}
        self.assertTrue(held(300_000, "claude-opus-5-5", ms("claude-opus-5-5", 333000)))
        self.assertFalse(held(240_000, "claude-opus-5-5", ms("claude-opus-5-5", 333000)))  # not near it yet
        self.assertTrue(held(300_000, "claude-opus-5-5-20260815", ms("claude-opus-5-5", 333000)))
        self.assertTrue(held(300_000, "us.anthropic.claude-opus-5-5-v1:0", ms("claude-opus-5-5", 333000)))
        self.assertFalse(held(300_000, "claude-opus-5-5", ms("claude-opus-5", 333000)))  # another model's key
        self.assertFalse(held(300_000, "claude-opus-5-5", {}))  # nothing set: the model's own 1M window
        auto = {"autoCompactWindow": 333000, "modelSettings": {"claude-opus-5-5": {"autoCompactWindow": "auto"}}}
        self.assertFalse(held(300_000, "claude-opus-5-5", auto))  # "auto": the model's own 1M window
        # /autocompact 733k: it starts at about 700k, and the safety limit follows, 150k later.
        self.assertTrue(held(700_000, "claude-opus-5-5", ms("claude-opus-5-5", 733000)))
        self.assertFalse(held(860_000, "claude-opus-5-5", ms("claude-opus-5-5", 733000)))
        # "auto" starts at about 967k, past 90% of the window: never held.
        self.assertFalse(held(970_000, "claude-opus-5-5", ms("claude-opus-5-5", "auto")))

    def test_plan_mode_is_not_held(self):
        self.run_hook("UserPromptSubmit", prompt="plan it", permission_mode="plan")
        self.tool()
        self.assertFalse(self.blocked(self.compact()))
        self.assertIsNone(self.run_hook("Stop", stop_hook_active=False, permission_mode="plan"))

    # ---- read-only agents and hold budgets

    def test_explore_is_read_only(self):
        self.tokens("main", 50_000)
        self.tokens("exp", 300_000)
        self.assertIsNone(self.run_hook("SubagentStart", "exp", agent_type="Explore"))  # no handoff path
        self.tool("exp")
        self.assertFalse(self.blocked(self.compact()))
        self.assertNotIn("just compacted", self.returned())
        self.assertEqual(self.ctx(self.tool("exp")), "")

    def test_custom_agent_is_held(self):
        # Only Explore and Plan are left alone: an agent without Write or Edit can still write with Bash.
        self.tokens("main", 50_000)
        self.tokens("rev", 300_000)
        self.assertIn("rev.md", self.ctx(self.run_hook("SubagentStart", "rev", agent_type="art-reviewer")))
        self.hold("rev")

    def test_subagent_is_held_until_its_safety_limit_like_main(self):
        self.tokens("main", 50_000)
        self.tokens("agentA", 300_000)
        for _ in range(20):
            self.hold("agentA")
        self.tokens("agentA", 455_000)
        self.tool("agentA")
        self.assertFalse(self.blocked(self.compact()))
        self.assertIn("before a handoff was written", self.returned("agentA"))

    # ---- trimming

    def big_handoff(self):
        self.write_handoff("main", "Owner: main session - big\n" + "x" * 12_000)

    def test_long_handoff_is_trimmed_once(self):
        self.hold()
        self.big_handoff()
        self.tool()
        self.assertTrue(self.blocked(self.compact()))  # held for a trim
        trim = self.ctx(self.tool())
        self.assertIn("over the 9,000 limit", trim)
        self.assertEqual(self.ctx(self.tool()), "")  # asked once
        self.assertTrue(self.blocked(self.compact()))  # still waiting for the slot to be written again
        self.write_handoff("main", "Owner: main session - trimmed")
        self.tool()
        self.assertTrue(self.shortens_summary(self.compact()))
        self.assertIn("trimmed", self.returned())

    def test_rewritten_long_handoff_goes_through_and_is_read_from_file(self):
        self.hold()
        self.big_handoff()
        self.tool()
        self.assertTrue(self.blocked(self.compact()))
        time.sleep(0.01)
        self.big_handoff()  # written again, still long: compaction goes ahead
        self.tool()
        self.assertTrue(self.shortens_summary(self.compact()))
        injected = self.returned()
        self.assertTrue(injected.startswith(NOTE))
        self.assertIn("use the Read tool", injected)
        self.assertLess(len(injected), 1200)

    def test_trim_waits_until_the_safety_limit(self):
        self.hold()
        self.big_handoff()
        reminders = 0
        for _ in range(20):  # however many requests the pruning takes
            reminders += "Reminder" in self.ctx(self.tool())
            self.assertTrue(self.blocked(self.compact()))
        self.assertGreaterEqual(reminders, 3)
        self.tokens("main", 460_000)
        self.tool()
        out = self.compact()
        self.assertFalse(self.blocked(out))
        self.assertFalse(self.shortens_summary(out))  # the full summary, as a fallback
        injected = self.returned()
        self.assertIn("still being pruned", injected)
        self.assertIn("_before-trim.md", injected)  # where the full version is

    def test_long_handoff_first_seen_at_the_limit_is_complete(self):
        self.hold()
        self.big_handoff()
        self.tokens("main", 460_000)
        self.tool()
        out = self.compact()
        self.assertFalse(self.blocked(out))
        self.assertTrue(self.shortens_summary(out))  # complete, just long: no trim was ever started
        self.assertNotIn("still being pruned", self.returned())

    def test_slow_trim_isnt_mistaken_for_a_stale_handoff(self):
        self.hold()
        self.big_handoff()
        self.tool()
        self.assertTrue(self.blocked(self.compact()))
        old = time.time() - 900
        os.utime(os.path.join(self.d, "main.md"), (old, old))  # the pruning has taken 15 minutes
        self.tool()
        self.assertTrue(self.blocked(self.compact()))
        self.assertNotIn("compaction is waiting for your handoff. Don't", self.ctx(self.tool()))
        self.assertEqual(len([f for f in self.archived() if "before-trim" in f]), 1)

    def test_stop_asks_for_trim(self):
        self.hold()
        self.big_handoff()
        self.tool()
        self.compact()
        out = self.run_hook("Stop", stop_hook_active=False)
        self.assertTrue(self.blocked(out))
        self.assertIn("over the 9,000 limit", out["reason"])

    # ---- project handoff files

    def test_existing_project_handoff_stays_in_place(self):
        handoff = self.project_handoff("# Game handoff\nRESUME HERE: beard A")
        self.hold()
        with open(handoff, "a") as f:
            f.write("\nowner said hi")  # routine edits to it don't release compaction
        self.tool()
        self.assertTrue(self.blocked(self.compact()))
        self.write_handoff("main", handoff)  # the slot names it: ready
        self.tool()
        self.assertTrue(self.shortens_summary(self.compact()))
        injected = self.returned()
        self.assertIn("RESUME HERE: beard A", injected)
        self.assertIn(handoff + " (via " + os.path.join(self.d, "main.md") + ")", injected)  # names the slot
        self.assertEqual(len(self.archived()), 1)
        self.tool()
        self.tokens("main", 300_000)
        self.hold()
        self.assertIn("This session's handoff file is " + handoff, self.ctx(self.tool()))

    def test_long_project_handoff_is_copied_then_trimmed_in_place(self):
        handoff = self.project_handoff("# Game handoff\n" + "y" * 20_000)
        self.hold()
        self.write_handoff("main", handoff)
        self.tool()
        self.assertTrue(self.blocked(self.compact()))
        before = [f for f in self.archived() if f.endswith("_before-trim.md")]
        self.assertEqual(len(before), 1)  # the long version is kept before it's pruned
        trim = self.ctx(self.tool())
        self.assertIn(handoff + " is 20015 characters", trim)
        self.assertIn("a copy is at", trim)
        self.assertIn("until your context reaches about 450k tokens", trim)
        with open(handoff, "w") as f:
            f.write("# Game handoff\npruned")  # edits alone don't release it...
        self.tool()
        self.assertTrue(self.blocked(self.compact()))
        time.sleep(0.01)
        self.write_handoff("main", handoff)  # ...writing the slot again does
        self.tool()
        self.assertTrue(self.shortens_summary(self.compact()))
        self.assertIn("pruned", self.returned())

    def test_slot_pointing_at_other_files_is_not_followed(self):
        secret = os.path.join(self.tmp.name, "secret.txt")
        with open(secret, "w") as f:
            f.write("TOPSECRET")
        self.hold()
        self.write_handoff("main", secret)
        told = self.ctx(self.tool())
        self.assertIn("not one to a handoff file", told)  # not followed, and the agent is told why
        self.assertTrue(self.blocked(self.compact()))
        self.assertEqual(self.ctx(self.tool()), "")  # told once per write
        out = self.run_hook("Stop", stop_hook_active=False)
        self.assertNotIn("TOPSECRET", json.dumps(out))

    def test_slot_path_in_quotes_or_relative_is_followed(self):
        for form in ("`%s`", '"%s" ', "docs/HANDOFF.md", "./docs/HANDOFF.md"):
            self.tearDown()
            self.setUp()
            handoff = self.project_handoff("# Game handoff\nRESUME HERE")
            relative = "%s" not in form
            written = form if relative else form % handoff
            cwd = os.path.dirname(os.path.dirname(handoff)) if relative else "/work"
            self.run_hook("SessionStart", source="startup", cwd=cwd)
            self.hold()
            self.write_handoff("main", written)
            self.tool()
            self.assertTrue(self.shortens_summary(self.compact()), written)
            self.assertIn("RESUME HERE", self.returned(), written)

    def test_fifo_in_the_slot_does_not_hang(self):
        fifo = os.path.join(self.tmp.name, "handoff.md")
        os.mkfifo(fifo)
        self.hold()
        os.symlink(fifo, os.path.join(self.d, "main.md"))
        self.tool()
        self.run_hook("PreCompact", trigger="auto")  # must return within the timeout

    # ---- archive

    def test_archive_keeps_each_loaded_handoff_once(self):
        self.hold()
        self.write_handoff("main", "Owner: main session - first")
        self.tool()
        self.compact()
        self.compact()  # a second attempt for the same compaction
        self.assertEqual(len(self.archived()), 1)
        self.assertTrue(self.archived()[0].endswith("_11111111_main.md"))
        self.returned()
        self.tool()
        self.tokens("main", 460_000)  # next cycle ends at the safety limit: nothing to archive
        self.tool()
        self.compact()
        self.assertEqual(len(self.archived()), 1)
        os.utime(os.path.join(self.root, "archive"), (0, 0))
        self.run_hook("SessionStart", source="startup")  # pruning leaves the archive alone
        self.assertEqual(len(self.archived()), 1)

    # ---- facts saved at compaction

    def said(self, agent, text, **extra):
        entry = {"type": "user", "message": {"role": "user", "content": text}}
        entry.update(extra)
        self.append(agent, entry)

    def test_facts_are_saved_beside_the_handoff_not_in_it(self):
        repo = os.path.dirname(os.path.dirname(self.project_handoff("# x\n", git=True)))
        for n in range(300):  # a long status stays in the file
            with open(os.path.join(repo, "untracked-%03d.txt" % n), "w") as f:
                f.write("x")
        self.run_hook("SessionStart", source="startup", cwd=repo)
        self.said("main", "port the parser, keep the old API", origin={"kind": "human"})
        self.said("main", "<task-notification>done</task-notification>", origin={"kind": "task-notification"})
        self.said("main", "a reminder", isMeta=True)
        self.hold()
        self.write_handoff("main", "Owner: main session - port")
        self.tool()
        self.compact()
        back = self.returned()
        facts = os.path.join(self.d, "main.facts.md")
        self.assertIn("Read it at " + facts, back)
        self.assertNotIn("untracked-", back)  # the listing is only in the file
        with open(facts) as f:
            text = f.read()
        self.assertIn("port the parser, keep the old API", text)
        self.assertNotIn("task-notification", text)
        self.assertNotIn("a reminder", text)
        self.assertIn("untracked-299.txt", text)
        self.assertIn("## Git, in " + repo, text)
        self.assertIn("git log --oneline -5", text)

    def test_facts_keep_the_latest_three_messages_and_cut_long_ones(self):
        for n in range(5):
            self.said("main", "request %d" % n, origin={"kind": "human"})
        self.said("main", "y" * 5000, origin={"kind": "human"})
        self.tool()
        self.compact("manual")
        self.returned()
        with open(os.path.join(self.d, "main.facts.md")) as f:
            text = f.read()
        self.assertNotIn("request 2", text)
        self.assertIn("request 3", text)
        self.assertLess(text.index("request 3"), text.index("request 4"))  # newest last
        self.assertIn("5000 characters in all", text)
        self.assertNotIn("## Git", text)  # /work isn't a repo

    def test_subagent_facts_hold_its_task(self):
        self.tokens("main", 50_000)
        self.tokens("agentA", 300_000)
        self.said("agentA", "Read f01.txt to f07.txt")  # a subagent's task has no origin
        self.said("agentA", "Follow-up: also f08.txt", isMeta=True, origin={"kind": "coordinator"})
        self.run_hook("SubagentStart", "agentA", agent_type="builder")
        self.hold("agentA")
        self.write_handoff("agentA", "Owner: subagent")
        self.tool("agentA")
        self.compact()
        self.assertIn("agentA.facts.md", self.returned("agentA"))
        with open(os.path.join(self.d, "agentA.facts.md")) as f:
            text = f.read()
        self.assertIn("The latest messages this agent was given", text)
        self.assertIn("Read f01.txt to f07.txt", text)
        self.assertIn("also f08.txt", text)

    # ---- input handling

    def test_compaction_summarizer_is_ignored(self):
        self.assertIsNone(self.run_hook("SubagentStart", "helper", agent_type=""))
        self.assertIsNone(self.run_hook("SubagentStop", "helper", agent_type=""))

    def test_bad_input_never_fails(self):
        for bad in ("not json", "{", '{"hook_event_name": "PostToolBatch", "session_id": "../../etc"}', "[]", ""):
            self.assertIsNone(self.run_raw(bad))

    def test_large_tool_output_is_skipped_quickly(self):
        self.hold()
        big = json.dumps({"session_id": SID, "transcript_path": self.transcript, "cwd": "/work", "tool_calls": [
                              {"tool_input": {"content": "a\\\"b" * 1_000_000},
                               "tool_response": {"nested": [{"x": "y" * 1000}] * 5000, "s": "\\u00e9\\" * 10}}],
                          "hook_event_name": "PostToolBatch"})
        start = time.time()
        out = self.run_raw(big)
        self.assertLess(time.time() - start, 0.5)
        self.assertIn("compaction is waiting", self.ctx(out))  # the fields after the big ones were read

    def test_escape_heavy_strings_are_skipped(self):
        for body in ('a\\"b' * 300_000, '\\\\' * 100_000 + '\\"', 'q\\\\\\"' * 50_000):
            raw = '{"tool_response": "%s", "hook_event_name": "UserPromptSubmit", "session_id": "%s"}' % (body, SID)
            json.loads(raw)  # valid JSON, so the fields after it must be found
            self.hold()
            self.assertIn("compaction is waiting", self.ctx(self.run_raw(raw)))
            self.tearDown()
            self.setUp()

    def test_settled_session_does_not_rewrite_state(self):
        self.hold()
        self.write_handoff("main", "Owner: main session - x")
        self.tool()
        self.compact()
        self.returned()
        self.tool()
        state = os.path.join(self.d, "state.json")
        os.utime(state, (0, 0))
        self.tool()
        self.assertEqual(os.path.getmtime(state), 0)  # most tool calls only touch the event file


if __name__ == "__main__":
    unittest.main()
