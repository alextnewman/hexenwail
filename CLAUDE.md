# Project Instructions for AI Agents

This file provides instructions and context for AI coding agents working on this project.

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:6cd5cc61 -->
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
   git push
   git status
   ```
5. **Hand off** - Summarize changes, validation, issue status, and any blocked sync/commit/push step

**Critical rules:**
- Explicit user or orchestrator instructions override this Beads block.
- Do not commit or push without clear authority from the active profile or the current user request.
- If a required sync or push is blocked, stop and report the exact command and error.
<!-- END BEADS INTEGRATION -->


## Canonical design documents

Before changing anything under `engine/` or `web/`, read these. They are the
agreed design; they exist so sessions stop re-deriving context from scratch.

| Document | Covers |
| --- | --- |
| [`docs/web/ARCHITECTURE.md`](docs/web/ARCHITECTURE.md) | What the web port is, its non-goals, layering, the `WEBQUAKE` / `WEBGL2QUAKE` macro contract, ownership boundaries |
| [`docs/web/SOFTWARE_RENDERER.md`](docs/web/SOFTWARE_RENDERER.md) | The **default** renderer: classic 8bpp rasteriser presented on an accelerated canvas, resolution ladder, iPad Pro panel math, cvars |
| [`docs/WEBGL_RENDERER.md`](docs/WEBGL_RENDERER.md) | The retained WebGL2 renderer (`-DWEB_RENDERER=webgl2`) |
| [`docs/PWA.md`](docs/PWA.md) | PWA shell, asset import, deployment |

**Settled decisions — do not reopen without an explicit instruction:**

* The target is a working uHexen2 in an installed **iOS PWA**. An
  Ironwail-class modern renderer is a **non-goal**.
* The web platform is treated as an independent OS, like a console port.
  POSIX/desktop parity and upstreamability are **non-goals**.
* The **software renderer is the default**. The WebGL2 renderer stays in-tree
  and buildable; do not delete it.

If the code and these documents disagree, say so explicitly and fix one of
them — do not silently invent a third plan.

## Build & Test

_Add your build and test commands here_

```bash
# Example:
# npm install
# npm test
```

## Architecture Overview

_Add a brief overview of your project architecture_

## Conventions & Patterns

_Add your project-specific conventions here_
