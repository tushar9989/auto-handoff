# auto-handoff

A Claude Code hook for long sessions. When auto-compaction is about to run, it holds it until the agent
has reached a clean point and written a handoff, then loads that handoff into the fresh context. It
replaces the manual "write a handoff, `/compact`, read it back" loop, for the main session and for every
subagent.

## Install

```sh
make install     # builds, tests and installs it
make uninstall   # removes exactly what install added
```

Installing registers the hook in `~/.claude/settings.json` (backed up first) and allows it to write to
`~/.local/state/claude-handoffs/`. Nothing else changes. Needs a C++17 compiler and Python 3.

Switch it on in any session with `/autocompact 333k`. Compaction then starts about 33k below the
window, at about 300k tokens; `/autocompact 733k` would start it at about 700k. The setting applies to
the model you're using, in that session and in sessions started or resumed after it. Sessions already
open keep the window they started with, so run it in each one you want changed. The hook doesn't need
to know a session's window: it acts when Claude Code starts a compaction, for any model with a window
set. `/autocompact auto`, or a window above about 890k, switches it off again.

## How it works

1. **Hold.** When compaction is about to run, the hook blocks it and asks the agent to finish the step
   it's on and write its handoff. It asks with the agent's next tool results, its next prompt, or at
   the end of the turn.
2. **Write.** Each agent writes to its own slot, `~/.local/state/claude-handoffs/<session>/<agent>.md`.
   A main session that already keeps a handoff file, such as `HANDOFF.md`, updates that and puts its
   path in the slot.
3. **Release.** Writing the slot lets compaction through, with Claude Code's summary cut to under 100
   words, since the handoff has the details.
4. **Load.** The fresh context opens with "A note from the user: make no mistakes. I love you.", then
   the handoff.
5. **Safety limit.** 150k past where compaction first came (450k for a 300k start), and at most at 900k,
   compaction goes through without a handoff rather than risk a failed request.

The handoff request asks for a rewrite rather than an append: where things stand now, finished work
in at most 10 lines, lasting knowledge moved to the project's docs. A handoff over 9,000 characters is
sent back once to be pruned. Loaded handoffs are archived in `~/.local/state/claude-handoffs/archive/`.
When the hook lets a compaction through, it also saves a facts file beside the slot: the last three
messages the agent was given, word for word, and the git state.

## Which agent is compacting

Since Claude Code 2.1.325, a subagent's PreCompact carries its `agent_id` and the main session's
carries none, so the hook knows which agent is compacting
([anthropics/claude-code#91910](https://github.com/anthropics/claude-code/issues/91910)). Once a
session has shown an id, a PreCompact without one is the main session's.

Before that, Claude Code didn't say, so the hook goes by timing until it sees an id. The hooks that run
just before each request do carry the agent's id: after each batch of tool calls, on a prompt, and on a
Stop the hook blocks. There, the hook notes the agent's size, counted the way Claude Code counts it.
PreCompact follows the compacting agent's own request within about 40ms, so the hook takes the agent
heard from in the last few seconds that is furthest past its compaction point. With parallel subagents
this can pick the wrong one (seen live on 2.1.325, where the id now prevents it). Each agent still gets
its own handoff back: handoffs are labelled by owner, and an agent that wasn't compacted goes back to
waiting.

## Other cases

- **Not held:** Explore and Plan subagents, agents in plan mode (they can't write files), and models
  with a 200k window (too little room to wait). These compact as usual.
- **`/compact`** always goes through, and loads a handoff if one was written.
- **Idle compaction** (a Claude Code default) compacts sessions of 200k or more when you come back after
  about an hour away. It saves re-reading the full context once the cache has expired. Below your
  threshold, the hook puts it off without asking for anything, and the session compacts at your
  threshold as usual, with its handoff. You can turn idle compaction off in `/config`.
- **Summary precompute** writes the summary in the background, about 34k before compaction is due (on a
  333k window), and swaps it in at the threshold. Since Claude Code 2.1.325 the start runs PreCompact,
  but the swap doesn't. Below your threshold the hook puts the precompute off, so the compaction at the
  threshold goes the usual way: held, handoff, loaded. Turning precompute off (`precomputeCompactionEnabled`
  set to false, or "Precompute compaction" in `/config`) skips those extra attempts.
- **A compaction well below the threshold** set in `settings.json`, by more than 10k, is put off the same
  way: for example, a session still on an older, lower window. It compacts at the threshold in
  `settings.json` instead.

## Limits

- The hook is on for a model when `settings.json` sets its window, which is what `/autocompact` writes. It
  ignores the `CLAUDE_CODE_AUTO_COMPACT_WINDOW` env var and per-project windows. A subagent whose
  definition sets its own lower `autoCompactWindow` can be mistaken for main, if main made a request in
  the same moment.
- Other PreCompact hooks run on every held attempt, so a slow one slows each request while a compaction
  waits.
- A handoff still over 9,000 characters after one prune goes through as it is.
- A compaction Claude Code starts after an API error (say an oversized image) is held like any other,
  and that request fails. It hasn't been seen in practice.

## Development

`make test` runs the unit tests. `make live` runs five real sessions on Sonnet, for about $3; re-run it
after Claude Code upgrades, since the hook relies on undocumented behaviour, described in comments in
`auto-handoff.cpp`. Set `AUTO_HANDOFF_LOG=/path` to log every decision. Each batch of tool calls adds
about 1ms, on top of starting the hook's process.

## License

Public domain, under the [Unlicense](LICENSE).
