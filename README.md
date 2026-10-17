# auto-handoff

A Claude Code hook that keeps sessions around 300k–400k tokens without the manual "write a handoff,
`/compact`, read it back" loop. Auto-compaction is held until the agent has written a handoff at a
clean point. The handoff is then loaded into the fresh context automatically.

## How it works

1. Auto-compact is set to try at about 300k tokens (`autoCompactWindow: 333000`; it fires about 33k
   below the window).
2. When it tries, the hook (PreCompact) works out which agent is compacting. It holds the compaction
   and notes that this agent owes a handoff.
3. On the agent's next tool result or prompt, the hook asks it to finish the step it's on and write
   its handoff. If a turn ends first, the Stop hook asks for it then. Each agent has a slot,
   `<session>/<agent>.md`:
   - **Main session with its own handoff file:** a session that already keeps one (say a
     `HANDOFF.md` in the project) updates it in place, as usual, and puts its path in the slot. The
     file stays where it is, and the hook remembers it for later reminders. Only files named like a
     handoff (`*handoff*.md`/`.txt`) are followed. The path may be absolute, relative to the
     session's folder, or wrapped in quotes or backticks. A path the hook can't follow doesn't count
     as written, and the agent is told why.
   - **Main session without one:** the handoff goes in the slot itself.
   - **Subagents:** they write their own handoff text to their slot. Handoff or progress files their
     own instructions ask for are kept as usual, separately.

   Writing the slot is the "ready" signal, so routine edits to a project handoff don't release
   compaction. A slot written more than 10 minutes before its compaction comes is treated as stale
   and asked for again.
4. Once the slot is written, the next attempt goes through. The hook asks Claude Code's summarizer
   for a summary under 100 words, since the handoff carries the details. It asks only if the
   conversation being summarized shows the agent being told to use that slot; each agent is told its
   own slot when it starts. Otherwise the summary is the usual full one.
5. Right after compaction, the hook loads the handoff into the fresh context: inline up to 9.5k
   characters, otherwise as a path to read. Every message that returns from a compaction opens with
   "A note from the user: make no mistakes. I love you.", even when there's nothing to load.
6. Safety limit: past 450k tokens (or 90% of the model's window) compaction goes through without a
   handoff, with Claude Code's full default summary. Holding all the way to the real limit would make
   the request fail.

### Which agent is compacting

Claude Code's compaction hooks don't say which agent is compacting, so the hook decides as follows:
- It takes the agent active in the last 2 seconds whose context is within 50k tokens of its own
  compaction point.
- Failing that, it takes the largest context near its point among agents active in the last 20
  minutes.
- If no agent is near its point, it doesn't hold.
- Agents whose size is unknown, and agents whose transcript shows they were just compacted, are
  never chosen.

The compaction point is read the way Claude Code reads it:
- `CLAUDE_CODE_AUTO_COMPACT_WINDOW` comes first. Values like `333k` are understood, and the value
  is held to 100k–1M.
- Next is the model's own `modelSettings` entry in `~/.claude/settings.json`. It matches by
  canonical name, so dated, `[1m]`, Bedrock and Vertex spellings count; `"auto"` means the model's
  own window.
- Otherwise the top-level `autoCompactWindow` applies.
- A subagent definition's own `autoCompactWindow` caps all of these, unless the env var is set.
- `CLAUDE_AUTOCOMPACT_PCT_OVERRIDE` is applied last.

The context size is the last response's input plus its output. Workflow agents' transcripts are
found wherever they live.

When the pick is wrong, nothing is lost:
- **The summary stays full.** The summarizer only shortens it when the conversation being compacted
  names that handoff's slot, and no other agent is ever told it.
- **Every loaded handoff says whose it is** ("the main session", "a subagent (pixel-builder)"). The
  agent continues from its own and ignores the rest.
- **Other ready handoffs are listed.** After compaction, the hook also lists handoffs that are ready
  but weren't released, so the agent that really compacted can find its own.
- **Agents are checked after a release.** An agent released but not compacted (its transcript shows
  no compaction) goes back to waiting, with its handoff still ready.
- **Missed handoffs are pointed out.** An agent that was compacted while its handoff wasn't loaded is
  told where its handoff is at its next step.

### Subagents

- **Read-only agents are never held**, since they couldn't write a handoff anyway; they compact
  normally. That covers:
  - agents whose definition's `tools` (an inline list, a `[...]` list or a YAML block list) has no
    Write, Edit or MultiEdit;
  - agents whose `disallowedTools` removes all three;
  - Explore and Plan.

  A definition with `memory` gets Write and Edit back, so it isn't read-only.
- **Hold budget:** any other subagent is held for at most 12 attempts.

### Keeping handoffs small

The request carries a housekeeping protocol:
- rewrite the handoff rather than append to it;
- record where things stand now, not the history;
- collapse finished work into a "Done so far" list of at most 10 lines;
- drop what's resolved or superseded;
- move lasting knowledge into the project's docs or memory (subagents put it in their final report
  instead).

It aims for 3,000–6,000 characters. A handoff over 9,000 characters holds compaction until it's
pruned and the slot is written again:
- **Budget:** the hold lasts until the context reaches the safety limit, so a 200KB project handoff
  can be read and pruned over many steps. The agent is told that limit and reminded every few steps.
- **Copy first:** the full version is copied to the archive before pruning starts.
- **Big moves can wait:** if moving knowledge out of it is a big job, the agent notes what still has
  to move and does it after compaction.
- **If the limit comes first:** compaction goes ahead with the handoff as it stands and the full
  summary. The returning agent is told the handoff may be half-edited and where the full version is.

### Archive

Loaded handoffs go to `~/.local/state/claude-handoffs/archive/<project
folder>/<date>_<time>_<session>_<agent>.md` and are kept for good, each one once. Handoffs being
pruned also get a `_before-trim` copy. A project handoff that git has committed and unchanged isn't
copied, because its git history is the archive.

### Other cases

- `/compact` always goes through, and loads a handoff if one was written since the last compaction.
- `/clear` loads the handoff of the session that was just cleared, linked through its SessionEnd
  hook, never another session's in the same folder.
- In plan mode the main session can't write files, so its compaction isn't held and Stop doesn't ask.
- Models with a 200k window aren't held. They compact only about 30k below their limit, which
  leaves no room to wait safely. They compact as usual, without a handoff.
- Idle compaction and background summary precompute are turned off by the installer, because both
  would compact behind the hook's back. If they're on anyway, idle compaction is held unless a
  handoff is ready.

The summary can't be switched off without switching off compaction entirely (`DISABLE_COMPACT`).
Claude Code also keeps a recent stretch of messages word for word, and reloads CLAUDE.md, memory and
up to five recently used files. None of that can be configured.

## Install

```sh
make install     # builds, runs the tests, installs
make uninstall   # undoes what install changed
```

Installing changes `~/.claude/settings.json`, after backing it up next to itself:

- copies the binary to `~/.claude/hooks/auto-handoff` and registers it for ten hook events
- allows Read/Edit/Write on `~/.local/state/claude-handoffs/**`
- sets `autoCompactWindow` to 333000 for every model family with a 1M window (Opus, Sonnet and
  Haiku 5.5, Fable, Mythos, Opus 5)
- sets `idleCompaction` and `precomputeCompactionEnabled` to false

What it changed is recorded in `~/.claude/hooks/auto-handoff.install.json`:
- **Uninstall** puts back only those values. Anything you've changed since, like a later
  `/autocompact`, stays.
- **Installing again** (say after a rebuild) keeps those later changes too.
- **Your file is edited in place.** A symlinked `settings.json` stays a symlink and keeps its
  permissions.
- **Unexpected settings are refused.** If `settings.json` isn't valid JSON or has an unexpected
  shape, nothing is changed and the script says where.

The new settings also reach sessions that are already running. A running session past 450k tokens
compacts at its next step without a handoff, so install between sessions.

Needs a C++17 compiler (Apple clang is fine) and Python 3 for the installer and tests.

## Files

`~/.local/state/claude-handoffs/<session_id>/` holds the slots (`main.md`, `<agent_id>.md`) and the
hook's state, kept for 14 days. The archive sits next to it and isn't pruned. None of this is under
`~/.claude`, because Claude Code treats writes there as sensitive and asks every time, even with an
allow rule.

## Tuning and debugging

- Limits, timings and every message text are constants at the top of `auto-handoff.cpp`. The window
  is in `install.py`.
- `AUTO_HANDOFF_LOG=/path/to/file` logs every decision ("compaction of main held (301k tokens)",
  "nudge", "allowed: handoff"). Errors go to `~/.local/state/claude-handoffs/errors.log`.
- `AUTO_HANDOFF_CEILING`, `AUTO_HANDOFF_DIR` and `AUTO_HANDOFF_SETTINGS` override the safety limit,
  the folder and the settings file the window is read from.
- Speed:
  - Each call costs about 0.7ms on top of starting a process.
  - Most calls only touch one file; state is locked and rewritten only while a compaction waits.
  - Tool output in the hook's input isn't parsed, so a 26MB Edit of a JSON file adds about 20ms.

## Tests

- `make test`: unit tests for the hook (driving the binary with hook events) and for the installer
  (against a throwaway config folder).
- `make live`: six scenarios in real headless sessions on Sonnet with a 100k window. Each run uses a
  fresh folder under `~/.local/state/claude-handoffs-livetest/` and deletes nothing. Costs about
  $3.30.
  - main session: reminded mid-turn;
  - Stop hook: asks for the handoff at the end of a turn;
  - parallel subagents: each gets its own handoff back;
  - model settings: subagents follow the per-model window;
  - project handoff: updated in place;
  - two cycles: the second handoff is a rewrite.

Re-run `make live` after upgrading Claude Code. This relies on behaviour measured on 2.1.302, much
of it undocumented:

- Auto-compact is checked before every model request, mid-turn included, and a held compaction is
  retried at the next one.
- PreCompact and SessionStart(compact) carry no agent id or permission mode. PreCompact follows the
  compacting agent's PostToolUse within milliseconds, and SessionStart(compact) context reaches the
  agent that compacted.
- Each compaction writes a `compact_boundary` line into the compacted agent's own transcript.
- The summarizer sees only the earlier part of the conversation. A recent stretch is kept word for
  word after the summary, so the summarizer never sees the handoff being written.
- Hook context over 10,000 characters is moved to a file.
- PreCompact's plain stdout, when it doesn't block, is appended to the summary prompt, for subagents
  too. The docs only describe blocking.
- Subagents honour the per-model `autoCompactWindow`. Their transcripts live under
  `<session>/subagents/`, workflow agents' one level deeper.

## Limits

- **Two agents at once:** if two agents both near their compaction points act within 2 seconds of
  each other, the hold or release can land on the wrong one. See "Which agent is compacting" for how
  that's recovered.
- **Blocked tool calls:** a tool call that another PreToolUse hook blocks leaves no event behind. The
  agent that made it is then found by size alone.
- **Settings the hook can't see:** a project's own `autoCompactWindow`, one passed with
  `--settings`, and windows tuned by the server.
- **Other projects' PreCompact hooks** run on every held attempt too, so a slow one slows each
  request while a compaction waits. Their SessionStart(compact) output can also come before the note.
- **200k-window models** get no automatic handoff (see "Other cases").
