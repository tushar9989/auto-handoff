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
the model you're using, in every session. `/autocompact auto`, or a window above about 890k, switches it
off again.

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
5. **Safety limit.** 150k past the start (450k for a 300k start), or at 900k if that comes first,
   compaction goes through without a handoff rather than risk a failed request.

The handoff request asks for a rewrite rather than an append: where things stand now, finished work
in at most 10 lines, lasting knowledge moved to the project's docs. A handoff over 9,000 characters is
sent back once to be pruned. Loaded handoffs are archived in `~/.local/state/claude-handoffs/archive/`.
When the hook lets a compaction through, it also saves a facts file beside the slot: the last three
messages the agent was given, word for word, and the git state.

## Which agent is compacting

Claude Code doesn't tell a PreCompact hook which agent is compacting. When a subagent compacts, the
input carries the main session's id, even though Claude Code has the agent's id at hand. That's
[anthropics/claude-code#91910](https://github.com/anthropics/claude-code/issues/91910). Without the id,
a hook can't tell whose handoff to wait for or load. Guessing from which transcript changed last fails
with parallel subagents.

So each agent announces itself instead. The hooks that run just before each request do carry the
agent's id: after each batch of tool calls, on a prompt, and on a Stop the hook blocks. There, the hook
repeats Claude Code's own size check, and notes the agent whose next request will cross its compaction
point. PreCompact follows within about 40ms and takes that agent. If two agents cross in the same
instant and the wrong one is taken, each still gets its own handoff back: handoffs are labelled by owner,
and an agent that wasn't compacted goes back to waiting. Once #91910 is fixed, all of this becomes a
lookup of the agent's id.

## Other cases

- **Not held:** Explore and Plan subagents, agents in plan mode (they can't write files), and models
  with a 200k window (too little room to wait). These compact as usual.
- **`/compact`** always goes through, and loads a handoff if one was written.
- **Idle compaction** (a Claude Code default) compacts sessions of 200k or more when you come back after
  about an hour away. It saves re-reading the full context once the cache has expired, but it compacts
  below your threshold, usually without a handoff. You can turn it off in `/config`.
- **Summary precompute** writes the summary in the background before compaction is due. It's gated on
  Anthropic's side and may not be active for you. Going by Claude Code 2.1.302's code, a compaction
  that uses it never calls PreCompact, so nothing is held and no handoff is written. To be safe, set
  `precomputeCompactionEnabled` to false.

## Limits

- The hook reads only the window in `settings.json`, which is what `/autocompact` writes. It ignores
  the `CLAUDE_CODE_AUTO_COMPACT_WINDOW` env var and per-project windows. A subagent whose definition
  sets its own `autoCompactWindow` compacts without a handoff, and main may be held in its place.
- Other PreCompact hooks run on every held attempt, so a slow one slows each request while a compaction
  waits.
- A handoff still over 9,000 characters after one prune goes through as it is.
- A compaction Claude Code starts after an API error (say an oversized image), while a hold is on, is
  held too, and that request fails. It hasn't been seen in practice.

## Development

`make test` runs the unit tests. `make live` runs five real sessions on Sonnet, for about $3; re-run it
after Claude Code upgrades, since the hook relies on undocumented behaviour, described in comments in
`auto-handoff.cpp`. Set `AUTO_HANDOFF_LOG=/path` to log every decision. Each batch of tool calls adds
about 1ms, on top of starting the hook's process.

## License

Public domain, under the [Unlicense](LICENSE).
