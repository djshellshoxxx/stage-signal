# Contributing

The repo runs a worker model: each worker owns one module, works on its own branch, and does not merge. The rules are in `docs/WORKER_RULES.md`. This page is the short version.

## Scope of a change

- Touch only your module's files: `core/<module>/`, `tests/unit/<module>/` and `docs/specs/<module>.md`.
- Do not edit the root `CMakeLists.txt`, `core/base`, `core/model`, or another module. If you need a change there, describe it in the PR body.
- Keep dependencies to `base`, `model` and the edges in [Architecture](Architecture).
- No globals, no threads inside core modules, no exceptions across public APIs.

## Before you write code

1. Read `docs/ARCHITECTURE.md`, the spec sections your task cites, and `core/base/include/ss/base/*.hpp` plus `core/model`.
2. Write or update `docs/specs/<module>.md` first. Keep it short and implementable. End it with an Integration API section of 40 lines or fewer.
3. Mark anything unfinished under Status, honestly: done, partial or not done, and why.

## Implementation rules

- Real code with tests. No stubs that pretend to work and no TODO placeholders in shipped paths.
- If something needs a device or an OS you cannot test, implement the portable logic fully, put the device edge behind an interface, and say so in Status.
- Every acceptance criterion gets a doctest named `REQ-XYZ: ...`.
- Build with `-Wall -Wextra` and keep your code warning-free. Run the tests three times to catch flakiness.

## Branches and PRs

| Step | Convention |
|---|---|
| Branch | `feat/<module>` (never `main`) |
| Commit message | Imperative mood |
| Push | `git fetch origin main && git rebase origin/main && git push -u origin feat/<module>` |
| PR title | `<module>: <summary>` |
| PR into | `main` |
| Merge | Not by the worker. The orchestrator merges. |

PR body: what is implemented, REQ IDs covered, test names, Status, and anything needed from others.

### Trailers

Commit messages end with a blank line and then two trailer lines:

```text
Co-Authored-By: <model name> <noreply@anthropic.com>
Claude-Session: <session URL>
```

PR bodies end with the Claude Code line and the session URL, each on its own line:

```text
🤖 Generated with [Claude Code](https://claude.com/claude-code)

<session URL>
```

Use the exact lines your session gives you. The worker rules name the trailer format; the model name in it must match the session that wrote the commit.

## Final report from a worker

At most 120 words: PR number or branch, test count, what is complete, what is partial or absent and why, and anything needed from others.

## Things that will bite you

- The machine is shared. Build only your target.
- `gh pr create` may fail on GraphQL in some sessions. Use `gh api repos/<owner>/<repo>/pulls -f ...` instead.
- Keep user-facing copy free of em dashes. A lint test scans UI strings (APP-UI-11).
