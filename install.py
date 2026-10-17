"""Install or remove the auto-handoff hook in ~/.claude/settings.json.

Run: make install / make uninstall. Installing copies build/auto-handoff to ~/.claude/hooks/,
registers it for every hook event it handles, allows writes to the handoff folder, sets each model's
auto-compact window so compaction first tries at about 300k tokens, and turns off idle compaction
and background summary precompute (both compact behind the hook's back). What it changed is recorded
next to the binary, so uninstalling undoes exactly that and leaves later changes alone; installing
again (after a rebuild) keeps values changed since, like a later /autocompact. settings.json is backed
up first, and written in place: a symlinked settings.json stays a symlink and keeps its permissions.
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
EVENTS = ["SessionStart", "SessionEnd", "SubagentStart", "SubagentStop", "UserPromptSubmit", "PostToolUse",
          "PostToolUseFailure", "PermissionDenied", "Stop", "PreCompact"]
PERMISSIONS = ["%s(~/.local/state/claude-handoffs/**)" % t for t in ("Read", "Edit", "Write")]
# Auto-compact fires about 33k tokens below the window, so 333k first tries at about 300k.
WINDOW = 333000
# Every model family with a native 1M window, by the canonical names modelSettings uses.
MODELS = ["claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5", "claude-fable-5-1", "claude-fable-5",
          "claude-mythos-5-1", "claude-mythos-5", "claude-opus-5", "claude-sonnet-5"]
OFF = {"idleCompaction": False, "precomputeCompactionEnabled": False}
ABSENT = "<absent>"


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
    expect(settings.get("modelSettings", {}), dict, '"modelSettings"')
    for model in MODELS:
        expect(settings.get("modelSettings", {}).get(model, {}), dict, '"modelSettings.%s"' % model)


def remove_hooks(settings):
    hooks = settings.get("hooks", {})
    for event in list(hooks):
        kept = [e for e in hooks[event] if not ours(e)]
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
        "existed": {k: k in settings for k in ("hooks", "permissions", "modelSettings")},
        "allow_existed": "allow" in settings.get("permissions", {}),
        "added_allow": [], "windows": {}, "models_existed": {}, "settings": {},
    }
    record.setdefault("left", [])  # values changed after an install: the user's from then on
    os.makedirs(os.path.dirname(BIN), exist_ok=True)
    shutil.copy2(os.path.join(HERE, "build", "auto-handoff"), BIN)
    remove_hooks(settings)
    hooks = settings.setdefault("hooks", {})
    for event in EVENTS:
        hooks.setdefault(event, []).append({"hooks": [{"type": "command", "command": BIN, "timeout": 10}]})
    allow = settings.setdefault("permissions", {}).setdefault("allow", [])
    for rule in PERMISSIONS:
        if rule not in allow:
            allow.append(rule)
            record["added_allow"].append(rule)
    models = settings.setdefault("modelSettings", {})
    for model in MODELS:
        if model in record["left"]:
            continue
        if model in record["windows"]:
            if models.get(model, {}).get("autoCompactWindow") != WINDOW:  # changed since: leave it
                del record["windows"][model], record["models_existed"][model]
                record["left"].append(model)
                continue
        else:
            record["models_existed"][model] = model in models
            record["windows"][model] = models.get(model, {}).get("autoCompactWindow")
        models.setdefault(model, {})["autoCompactWindow"] = WINDOW
    if not models and not record["existed"]["modelSettings"]:
        del settings["modelSettings"]
    for key, value in OFF.items():
        if key in record["left"]:
            continue
        if key in record["settings"]:
            if settings.get(key) != value:
                del record["settings"][key]
                record["left"].append(key)
                continue
        else:
            record["settings"][key] = settings.get(key, ABSENT)
        settings[key] = value
    with open(RECORD, "w") as f:
        json.dump(record, f, indent=2)


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
        models = settings.get("modelSettings", {})
        for model, previous in record["windows"].items():
            entry = models.get(model)
            if entry is None or entry.get("autoCompactWindow") != WINDOW:
                continue  # changed since install: leave it
            if previous is None:
                del entry["autoCompactWindow"]
            else:
                entry["autoCompactWindow"] = previous
            if not entry and not record["models_existed"][model]:
                del models[model]
        if "modelSettings" in settings and not models and not record["existed"]["modelSettings"]:
            del settings["modelSettings"]
        for key, previous in record["settings"].items():
            if settings.get(key) != OFF[key]:
                continue
            if previous == ABSENT:
                del settings[key]
            else:
                settings[key] = previous
    for p in (BIN, RECORD):
        if os.path.exists(p):
            os.remove(p)


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
    (uninstall if removing else install)(settings)
    if exists or not removing:
        os.makedirs(os.path.dirname(target), exist_ok=True)
        tmp = target + ".tmp-auto-handoff"
        with open(tmp, "w") as f:
            json.dump(settings, f, indent=2)
            f.write("\n")
        if exists:
            os.chmod(tmp, os.stat(target).st_mode & 0o7777)
        os.replace(tmp, target)
    print("%s.%s" % ("Uninstalled" if removing else "Installed",
                     " Backup of the previous settings: " + backup if backup else ""))


if __name__ == "__main__":
    main()
