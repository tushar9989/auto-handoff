"""Install or remove the auto-handoff hook in ~/.claude/settings.json.

Run: make install / make uninstall. Installing copies build/auto-handoff to ~/.claude/hooks/, registers
it for every hook event it handles, and allows writes to the handoff folder. Nothing else changes: the
hook runs only for models with an auto-compact window set with /autocompact (see README.md). What it changed
is recorded next to the binary, so uninstalling undoes exactly that. settings.json is backed up first,
and written in place: a symlinked settings.json stays a symlink and keeps its permissions.
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CLAUDE = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.expanduser("~/.claude")
SETTINGS = os.path.join(CLAUDE, "settings.json")
BIN = os.path.join(CLAUDE, "hooks", "auto-handoff")
RECORD = BIN + ".install.json"
EVENTS = ["SessionStart", "SubagentStart", "SubagentStop", "UserPromptSubmit", "PostToolBatch", "Stop", "PreCompact"]
PERMISSIONS = ["%s(~/.local/state/claude-handoffs/**)" % t for t in ("Read", "Edit", "Write")]


def ours(entry):
    return any(h.get("command") == BIN for h in entry.get("hooks", []))


def check(settings):
    """Refuse settings this script can't safely edit, before anything is written."""
    def expect(value, kind, where):
        if not isinstance(value, kind):
            sys.exit("settings.json: %s isn't %s; fix it, or install by hand (see README.md). Nothing was changed."
                     % (where, "an object" if kind is dict else "a list"))
    expect(settings, dict, "the file")
    expect(settings.get("hooks", {}), dict, '"hooks"')
    for event, entries in settings.get("hooks", {}).items():
        expect(entries, list, '"hooks.%s"' % event)
        for entry in entries:
            expect(entry, dict, 'an entry in "hooks.%s"' % event)
            expect(entry.get("hooks", []), list, 'the "hooks" of an entry in "hooks.%s"' % event)
            for h in entry.get("hooks", []):
                expect(h, dict, 'a hook in "hooks.%s"' % event)
    expect(settings.get("permissions", {}), dict, '"permissions"')
    expect(settings.get("permissions", {}).get("allow", []), list, '"permissions.allow"')


def remove_hooks(settings):
    """Take our hook out of every entry. An entry goes only when nothing else is left in it."""
    hooks = settings.get("hooks", {})
    for event in list(hooks):
        kept = []
        for e in hooks[event]:
            if ours(e):
                e["hooks"] = [h for h in e["hooks"] if h.get("command") != BIN]
                if not e["hooks"]:
                    continue
            kept.append(e)
        if kept or len(kept) == len(hooks[event]):
            hooks[event] = kept
        else:
            del hooks[event]


def load_record():
    if not os.path.exists(RECORD):
        return {}
    with open(RECORD) as f:
        return json.load(f)


def install(settings):
    record = load_record() or {
        "existed": {k: k in settings for k in ("hooks", "permissions")},
        "allow_existed": "allow" in settings.get("permissions", {}),
        "added_allow": [],
    }
    # A new file, swapped in: overwriting the binary in place while a hook runs it can leave macOS
    # killing it on every later run (the kernel keeps the old code signature for that file).
    os.makedirs(os.path.dirname(BIN), exist_ok=True)
    shutil.copy2(os.path.join(HERE, "build", "auto-handoff"), BIN + ".new")
    os.replace(BIN + ".new", BIN)
    remove_hooks(settings)
    hooks = settings.setdefault("hooks", {})
    for event in EVENTS:
        hooks.setdefault(event, []).append({"hooks": [{"type": "command", "command": BIN, "timeout": 10}]})
    allow = settings.setdefault("permissions", {}).setdefault("allow", [])
    for rule in PERMISSIONS:
        if rule not in allow:
            allow.append(rule)
            if rule not in record["added_allow"]:
                record["added_allow"].append(rule)
    return record


def uninstall(settings):
    record = load_record()
    remove_hooks(settings)
    if "hooks" in settings and not settings["hooks"] and not record.get("existed", {}).get("hooks", True):
        del settings["hooks"]
    if record:
        permissions = settings.get("permissions")
        if permissions is not None and "allow" in permissions:
            permissions["allow"] = [p for p in permissions["allow"] if p not in record["added_allow"]]
            if not permissions["allow"] and not record["allow_existed"]:
                del permissions["allow"]
        if permissions is not None and not permissions and not record["existed"]["permissions"]:
            del settings["permissions"]
    return None


def main():
    removing = "--uninstall" in sys.argv
    target = os.path.realpath(SETTINGS)  # write through a symlink, not over it
    exists = os.path.exists(target)
    settings = {}
    if exists:
        with open(target) as f:
            try:
                settings = json.load(f)
            except ValueError as e:
                sys.exit("settings.json isn't valid JSON (%s); fix it first. Nothing was changed." % e)
    check(settings)
    backup = None
    if exists:
        backup = "%s.bak-auto-handoff-%s" % (SETTINGS, time.strftime("%Y%m%d-%H%M%S"))
        n = 1
        while os.path.exists(backup):
            n += 1
            backup = "%s.bak-auto-handoff-%s-%d" % (SETTINGS, time.strftime("%Y%m%d-%H%M%S"), n)
        shutil.copy2(target, backup)
    record = (uninstall if removing else install)(settings)
    if exists or not removing:
        os.makedirs(os.path.dirname(target), exist_ok=True)
        tmp = target + ".tmp-auto-handoff"
        with open(tmp, "w") as f:
            json.dump(settings, f, indent=2)
            f.write("\n")
        if exists:
            os.chmod(tmp, os.stat(target).st_mode & 0o7777)
        os.replace(tmp, target)
    # The record goes last, so a failed settings write never leaves one describing changes not made.
    if removing:
        for p in (BIN, RECORD):
            if os.path.exists(p):
                os.remove(p)
    else:
        with open(RECORD, "w") as f:
            json.dump(record, f, indent=2)
    print("%s.%s" % ("Uninstalled" if removing else "Installed",
                     " Backup of the previous settings: " + backup if backup else ""))
    if not removing:
        print("It runs only for a model with an auto-compact window set: /autocompact 333k starts compaction at "
              "about 300k tokens.")


if __name__ == "__main__":
    main()
