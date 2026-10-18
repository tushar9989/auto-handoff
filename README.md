# auto-handoff

A Claude Code hook that keeps sessions around 300k–400k tokens without the manual "write a handoff,
`/compact`, read it back" loop. Auto-compaction is held until the agent has written a handoff at a
clean point. The handoff is then loaded into the fresh context automatically.

## How it works

1. You switch it on by setting an auto-compact window: `/autocompact 333k` makes compaction first try
   at about 300k tokens (it fires about 33k below the window). Until then it does nothing (see
   "Switching it on").
2. When it tries, the hook (PreCompact) works out which agent is compacting. It holds the compaction
   and notes that this agent owes a handoff.
3. On the agent's next batch of tool results or prompt, the hook asks it to finish the step it's on and write
   its handoff. If a turn ends first, the Stop hook asks for it then. Each agent has a slot,
   `<session>/<agent>.md`:
   - **Main session with its own handoff file:** a session that already keeps one (say a
     `HANDOFF.md` in the project) updates it in place, as usual, and puts its path in the slot. The
     file stays where it is, and the hook remembers it for later reminders. Only files named like a
     handoff (`*handoff*.md`/`.txt`) are followed. The path may be absolute, relative to the
     session's folder, or wrapped in quotes or backticks. A path the hook can't follow doesn't count
     as written, and the agent is told why.
   - **Main session without one:** the handoff goes in the slot itself.
   - **Subagents:** they write their own handoff text to their slot. A slot of theirs is never
     followed as a path, so a subagent is never asked to prune a project file it may share. Handoff or
     progress files their own instructions ask for are kept as usual, separately.

   Writing the slot is the "ready" signal, so routine edits to a project handoff don't release
   compaction. Once a handoff has been loaded (and archived), its slot is removed, so the next one is a
   new file: the Write tool won't overwrite a file the agent hasn't read in its current context.
4. Once the slot is written, the next attempt goes through. The hook asks Claude Code's summarizer
   for a summary under 100 words, since the handoff carries the details. It asks only if the
   conversation being summarized names that slot as the agent's own. Each agent is told its own slot
   when it starts, when it's asked for a handoff, and after each compaction, and no other agent is
   told it. Otherwise the summary is the usual full one.

   A handoff written well before its compaction comes (the context has grown more than 20k since,
   say because the agent was asked early and went on working) is asked for again first. If the
   context shrinks well below the compaction point without a compaction (a rewind), the agent is no
   longer asked.
5. Right after compaction, the hook loads the handoff into the fresh context: inline up to 9.5k
   characters, otherwise as a path to read. Every message that returns from a compaction opens with
   "A note from the user: make no mistakes. I love you.", even when there's nothing to load.
6. Safety limit: 150k tokens past the compaction point (450k when it starts at 300k), and never more
   than 90% of the model's window. Past it, compaction goes through without a handoff, with Claude
   Code's full default summary. Holding all the way to the real limit would make the request fail.

### Which agent is compacting

Claude Code's compaction hooks don't say which agent is compacting (issue
[#91910](https://github.com/anthropics/claude-code/issues/91910)), so each agent announces itself:
- **Before every request**, Claude Code checks whether the context has reached the compaction point.
  It sizes the context as the last response's input and output, plus everything added since (its
  tool calls, their results, a prompt), at 3 bytes per token (4 for older models), with 2,000 tokens
  for each image or document.
- **The hook does the same sum** on the events just before a request, which do say who the agent is.
  Those events are a batch of tool results (PostToolBatch, which carries the calls and the results),
  a new prompt, and a Stop the hook blocks. If the agent's next request reaches its compaction point (less 3k for
  small extras), the hook writes `<session>/crossing/<agent>`.
- **PreCompact follows within about 40ms**, so the newest announcement from the last 5 seconds is
  the compacting agent. Another agent would have to announce inside those 40ms to be taken instead.
- **If nobody announced**, the main session is the candidate when it's within 50k of its point.
  Examples are idle compaction and a background agent's report reaching main, which no prompt event
  announces.
- An agent past its safety limit, or with a small window, is never blocked: blocking either one could
  make a request fail.

Agents whose transcript shows they were just compacted have a small size again, so they don't
announce. If the latest response isn't in the transcript's last 256KB (say, after large tool
results), the hook reads back further, up to 32MB.

The compaction point is 33k below the window set for the model in `~/.claude/settings.json`: its
`modelSettings` entry (the one `/autocompact` writes; dated, Bedrock and Vertex model names count),
else the top-level `autoCompactWindow`, else the model's own window. `"auto"` means the model's own
window. Other ways of setting it (see Limits) aren't read; at worst, a compaction point the hook
gets wrong means that compaction happens as usual, without a handoff.

Workflow agents' transcripts are found wherever they live.

If the wrong agent is taken, nothing is lost:
- **The summary stays full.** The summarizer only shortens it when the conversation being compacted
  names that handoff's slot, and no other agent is ever told it.
- **Every loaded handoff says whose it is** ("the main session", "a subagent (pixel-builder)"), and
  names its slot. The agent continues from its own and ignores the rest.
- **Other ready handoffs are listed.** After compaction, the hook also lists handoffs that are ready
  but weren't released, so the agent that really compacted can find its own.
- **Agents are checked after a release.** An agent released but not compacted (its transcript shows
  no compaction) goes back to waiting, with its handoff still ready.

### Facts saved at compaction

When an agent's compaction goes through, the hook saves `<session>/<agent>.facts.md`. Nobody writes
it; the hook gathers it:
- the last three messages to the agent, word for word (yours for the main session; the task and the
  main session's follow-ups for a subagent), each cut at 4,000 characters;
- the branch and changed files (`git status --short --branch`);
- a per-file change summary (`git diff --stat HEAD`), and the last five commits.

Diffs are left out, since they can be huge, and each git listing is cut at 12KB. The returning agent
gets one line saying where the file is, and reads it only if it needs to. A git command taking over
2 seconds is skipped.

### Subagents

- **Explore and Plan agents are never held**, since they're told not to change files; they compact
  normally. Every other agent is held whatever its tools: one without Write or Edit can still write
  its handoff with Bash.
- **Every other subagent is held like the main session**: until its handoff is written (and trimmed),
  or until the safety limit.

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
  A long handoff first seen at the limit was never being pruned: it's loaded as it is.

### Archive

Loaded handoffs go to `~/.local/state/claude-handoffs/archive/<project
folder>/<date>_<time>_<session>_<agent>.md` and are kept for good, each one once, project handoff
files included. Handoffs being pruned also get a `_before-trim` copy.

### Other cases

- `/compact` always goes through, and loads a handoff if one was written since the last compaction.
- `/clear` starts afresh: a handoff written but not yet loaded stays in the old session's folder.
- An agent in plan mode, main or a subagent, can't write files, so its compaction isn't held and it
  isn't asked.
- Models with a 200k window aren't held. They compact only about 30k below their limit, which
  leaves no room to wait safely. They compact as usual, without a handoff.
- Claude Code's idle compaction and background summary precompute are left as they are (both are on
  unless you turn them off in `/config`):
  - **Idle compaction** compacts a session of 200k tokens or more at your first request after about
    54 minutes away, which saves re-reading the whole context at full price once the cache has
    expired. It compacts below your threshold and without a handoff, unless the session is within
    50k of its compaction point, when the hook holds it and asks for one. Turn it off if you'd rather
    keep the detail across breaks.
  - **Precompute** writes the summary in advance, so compaction is instant. A summary written in
    advance probably can't follow the request to keep it short, so it may come out full length next
    to the handoff. Turn it off if summaries come back long.

The summary can't be switched off without switching off compaction entirely (`DISABLE_COMPACT`).
Claude Code also keeps a recent stretch of messages word for word, and reloads CLAUDE.md, memory and
up to five recently used files. None of that can be configured.

## Install

```sh
make install     # builds, runs the tests, installs (off until you switch it on)
make uninstall   # undoes what install changed
```

Installing changes `~/.claude/settings.json`, after backing it up next to itself, and nothing else in
it:

- copies the binary to `~/.claude/hooks/auto-handoff` and registers it for seven hook events. A
  reinstall swaps in a new file rather than overwriting the old one: macOS can otherwise keep killing
  a binary that was rewritten while a hook was running it.
- allows Read/Edit/Write on `~/.local/state/claude-handoffs/**`

What it added is recorded in `~/.claude/hooks/auto-handoff.install.json`:
- **Uninstall** removes only that. Your auto-compact window and anything else you've changed stay.
- **Your file is edited in place.** A symlinked `settings.json` stays a symlink and keeps its
  permissions.
- **Only its own hook is removed.** A hook of yours that shares an entry with it stays.
- **Unexpected settings are refused.** If `settings.json` isn't valid JSON or has an unexpected
  shape, nothing is changed and the script says where.

## Switching it on

Installed, the hook does nothing: no messages, no holds, no note after compactions. It's on for a model
once that model has an auto-compact window that leaves room to wait, up to about 890k on a 1M model.
Run `/autocompact <window>` in a session: compaction starts about 33k below the window, so
`/autocompact 333k` starts it at about 300k and `/autocompact 733k` at about 700k. That saves the
window for the model you're using in your settings, so every session follows it, running ones
included, and so does the hook. A session already past the new safety limit compacts at its next
step without a handoff.

The safety limit moves with it: 150k past the start, at most 90% of the window (450k for a start at
300k, 850k for 700k).

To switch it off again, run `/autocompact auto`: the model's own window starts compaction at about
967k, past the safety limit, so compaction runs as it would without the hook. `make uninstall` removes
it completely.

Needs a C++17 compiler (Apple clang is fine) and Python 3 for the installer and tests.

## Files

`~/.local/state/claude-handoffs/<session_id>/` holds the slots (`main.md`, `<agent_id>.md`) and the
hook's state, kept for 14 days. The archive sits next to it and isn't pruned. None of this is under
`~/.claude`, because Claude Code treats writes there as sensitive and asks every time, even with an
allow rule.

## Tuning and debugging

- Limits, timings and every message text are constants at the top of `auto-handoff.cpp`. The window
  is set with `/autocompact`.
- `AUTO_HANDOFF_LOG=/path/to/file` logs every decision ("compaction of main held (301k tokens)",
  "nudge", "allowed: handoff"). Errors go to `~/.local/state/claude-handoffs/errors.log`.
- `AUTO_HANDOFF_DIR` and `AUTO_HANDOFF_SETTINGS` override the folder and the settings file the window
  is read from.
- Speed:
  - The hook runs once per batch of tool calls, not once per call. Each run costs about 0.9ms on top
    of starting a process, reading the last 256KB of the agent's transcript.
  - State is locked and rewritten only while a compaction waits.
  - Tool results in the hook's input are measured, not parsed: 2MB of them adds about 1ms.

## Tests

- `make test`: unit tests for the hook (driving the binary with hook events) and for the installer
  (against a throwaway config folder).
- `make live`: five scenarios in real headless sessions on Sonnet, with a 100k window set for the
  model in settings. Each run uses a fresh folder under `~/.local/state/claude-handoffs-livetest/`
  and deletes nothing. Costs about $3.
  - main session: reminded mid-turn;
  - Stop hook: asks for the handoff at the end of a turn;
  - parallel subagents: each gets its own handoff back, and follow the per-model window;
  - project handoff: updated in place;
  - two cycles: the second handoff is a rewrite.

Re-run `make live` after upgrading Claude Code. This relies on behaviour measured on 2.1.302, much
of it undocumented:

- Auto-compact is checked before every model request, mid-turn included, and a held compaction is
  retried at the next one.
- PreCompact and SessionStart(compact) carry no agent id or permission mode. PreCompact follows the
  compacting agent's PostToolBatch (which carries the id and the tool results) by about 40ms, and
  SessionStart(compact) context reaches the agent that compacted.
- The compaction check sizes the context the way "Which agent is compacting" describes (read in
  2.1.302's code; a live probe of Read batches matched it every time).
- Each compaction writes a `compact_boundary` line into the compacted agent's own transcript.
- The summarizer sees only the earlier part of the conversation. A recent stretch is kept word for
  word after the summary, so the summarizer never sees the handoff being written.
- Hook context over 10,000 characters is moved to a file.
- PreCompact's plain stdout, when it doesn't block, is appended to the summary prompt, for subagents
  too. The docs only describe blocking.
- Subagents honour the per-model `autoCompactWindow`. Their transcripts live under
  `<session>/subagents/`, workflow agents' one level deeper.

## Limits

- **Two agents crossing in the same instant:** if another agent's batch lands in the 40ms between an
  agent's announcement and its PreCompact, the hold or release goes to the wrong one. See "Which
  agent is compacting" for how that's recovered.
- **The cap is asked for once.** A handoff over 9,000 characters holds compaction until it's written
  again, but whatever is written then goes through, even if it's still over the cap.
- **Requests no hook announces** fall back to the main session when it's near its compaction point.
  These are a subagent's first request with a large inherited context, and messages that arrive
  without a prompt event. A subagent's compaction there goes through without its handoff. If main is
  near its point at that moment, main is asked for its handoff early instead, and the subagent's
  compaction waits until its next request, which it does announce. If main then goes on working,
  its handoff is asked for again once it's outgrown.
- **Settings the hook doesn't read:** `CLAUDE_CODE_AUTO_COMPACT_WINDOW`,
  `CLAUDE_AUTOCOMPACT_PCT_OVERRIDE`, a subagent definition's own `autoCompactWindow`, a project's own
  setting, one passed with `--settings`, and windows tuned by the server. Agents they apply to may
  compact as usual, without a handoff.
- **Other projects' PreCompact hooks** run on every held attempt too, so a slow one slows each
  request while a compaction waits. Pruning a big handoff can take dozens of requests. Their
  SessionStart(compact) output can also come before the note.
- **200k-window models** get no automatic handoff (see "Other cases").
- **A compaction Claude Code starts after an API error** (an image or request too large) while a hold
  is on gets held too, and that one request fails. This hasn't shown up in practice.
