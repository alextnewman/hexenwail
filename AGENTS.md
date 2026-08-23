# Agent Instructions

This project uses **bd** (beads) for issue tracking. Run `bd onboard` to get started.

## First-time setup (fresh clone)

The issue history is version-controlled in `.beads/issues.jsonl`; the local Dolt
database under `.beads/` is git-ignored, so a fresh clone has **no materialized
database** until you build one from the tracked JSONL. `bd onboard`/`bd init`
alone create an *empty* database — you must import the existing issues:

```bash
# Build the local database AND load all tracked issues in one step:
bd init --from-jsonl --non-interactive

# Already ran `bd init`? Just import into the existing database:
bd import            # reads .beads/issues.jsonl

bd ready             # verify: should list the open, unblocked issues
```

`bd` uses an embedded Dolt engine by default (no server required). After local
writes it re-exports to `.beads/issues.jsonl`; commit that file alongside code so
issue state stays in sync. Keep statuses to bd's valid set (`open`,
`in_progress`, `blocked`, `closed`) — non-standard values like `completed` or
`deferred` will fail to import for the next person.

## Canonical design documents

Before changing anything under `engine/` or `web/`, read these. They are the
agreed design; they exist so sessions stop re-deriving context from scratch.

| Document | Covers |
| --- | --- |
| [`docs/web/ARCHITECTURE.md`](docs/web/ARCHITECTURE.md) | What the web port is, its non-goals, layering, the `WEBQUAKE` / `WEBGL2QUAKE` macro contract, ownership boundaries |
| [`docs/web/SOFTWARE_RENDERER.md`](docs/web/SOFTWARE_RENDERER.md) | The **default** renderer: classic 8bpp rasteriser presented on an accelerated canvas, resolution ladder, iPad Pro panel math, cvars |
| [`docs/web/WEBGLIDE.md`](docs/web/WEBGLIDE.md) | The experimental WebGlide GPU renderer (`-DWEB_RENDERER=webgl2`) |
| [`docs/web/PERF_CAPTURE.md`](docs/web/PERF_CAPTURE.md) | Copyable raw performance capture shared by both renderers |
| [`docs/web/WEBGLIDE_NITRO.md`](docs/web/WEBGLIDE_NITRO.md) | **Idea only, gated:** console-style tricks layered on a finished WebGlide |
| [`docs/PWA.md`](docs/PWA.md) | PWA shell, asset import, deployment |

**Settled decisions — do not reopen without an explicit instruction:**

* The target is a working uHexen2 in an installed **iOS PWA**. An
  Ironwail-class modern renderer is a **non-goal**.
* The web platform is treated as an independent OS, like a console port.
  POSIX/desktop parity and upstreamability are **non-goals**.
* The **software renderer is the default**. The WebGlide GPU renderer stays
  in-tree and buildable; do not delete it.

If the code and these documents disagree, say so explicitly and fix one of
them — do not silently invent a third plan.

## Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --status in_progress  # Claim work
bd close <id>         # Complete work
bd sync               # Sync with git
```

## Landing the Plane (Session Completion)

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd sync
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds


<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:970c3bf2 -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/SYNC_CONCEPTS.md for details and anti-patterns.

## Agent Context Profiles

The managed Beads block is task-tracking guidance, not permission to override repository, user, or orchestrator instructions.

- **Conservative (default)**: Use `bd` for task tracking. Do not run git commits, git pushes, or Dolt remote sync unless explicitly asked. At handoff, report changed files, validation, and suggested next commands.
- **Minimal**: Keep tool instruction files as pointers to `bd prime`; use the same conservative git policy unless active instructions say otherwise.
- **Team-maintainer**: Only when the repository explicitly opts in, agents may close beads, run quality gates, commit, and push as part of session close. A current "do not commit" or "do not push" instruction still wins.

## Session Completion

This protocol applies when ending a Beads implementation workflow. It is subordinate to explicit user, repository, and orchestrator instructions.

1. **File issues for remaining work** - Create beads for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **Handle git/sync by active profile**:
   ```bash
   # Conservative/minimal/default: report status and proposed commands; wait for approval.
   git status

   # Team-maintainer opt-in only, unless current instructions forbid it:
   git pull --rebase
   bd dolt push
   git push
   git status
   ```
5. **Hand off** - Summarize changes, validation, issue status, and any blocked sync/commit/push step

**Critical rules:**
- Explicit user or orchestrator instructions override this Beads block.
- Do not commit or push without clear authority from the active profile or the current user request.
- If a required sync or push is blocked, stop and report the exact command and error.
<!-- END BEADS INTEGRATION -->

<!-- BEGIN BEADS CODEX SETUP: generated by bd setup codex -->
## Beads Issue Tracker

Use Beads (`bd`) for durable task tracking in repositories that include it. Use the `beads` skill at `.agents/skills/beads/SKILL.md` (project install) or `~/.agents/skills/beads/SKILL.md` (global install) for Beads workflow guidance, then use the `bd` CLI for issue operations.

### Quick Reference

```bash
bd ready                # Find available work
bd show <id>            # View issue details
bd update <id> --claim  # Claim work
bd close <id>           # Complete work
bd prime                # Refresh Beads context
```

### Rules

- Use `bd` for all task tracking; do not create markdown TODO lists.
- Run `bd prime` when Beads context is missing or stale. Codex 0.129.0+ can load Beads context automatically through native hooks; use `/hooks` to inspect or toggle them.
- Keep persistent project memory in Beads via `bd remember`; do not create ad hoc memory files.

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/SYNC_CONCEPTS.md for details and anti-patterns.
<!-- END BEADS CODEX SETUP -->
