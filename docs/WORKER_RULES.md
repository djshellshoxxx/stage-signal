# Worker rules (read fully before starting)

You own ONE module (named in your task). Work in your own git worktree/branch, never on `main`.

## Setup and loop
1. `docs/ARCHITECTURE.md`, then the spec sections your task cites in `StageSignal-Specification-v0.1.md`
   and `docs/spec-addendum-v0.2.md`. Read `core/base/include/ss/base/*.hpp` and `core/model` (shared contracts).
2. Branch `feat/<module>`. Build/test loop (core only, fast):
   `cmake -S . -B build -G Ninja -DSS_BUILD_APP=OFF && cmake --build build --target test_<module> && ./build/tests_<module>/test_<module>`
   The machine has 2 cores shared with other workers: build only your target, never the whole tree repeatedly.
3. Write `docs/specs/<module>.md` FIRST (short, implementable): scope, REQ ids, public API, data flow, limits,
   failure behaviour, tests. Finish it as you learn. End it with an **Integration API** section (<= 40 lines) that the
   app integrator can wire from without reading your code. Mark anything you could not finish under **Status** honestly
   (done / partial / not done + why). Do not claim hardware qualification you did not perform.
4. Implement real, working code with tests. No stubs that pretend to work, no TODO placeholders in shipped paths.
   If something needs a device/OS you cannot test, implement the portable logic fully and isolate the device edge
   behind an interface; say so under Status.
5. Only touch files under your own `core/<module>/`, `tests/unit/<module>/`, `docs/specs/<module>.md`.
   Need a change elsewhere (base/model/another module)? Do NOT edit it; describe the need in your PR body.
6. Keep dependencies to `base`, `model` and the edges allowed in ARCHITECTURE.md. C++17, MSVC/gcc/clang clean
   (no `-Werror`, but no warnings in your code), no compiler extensions, no exceptions across public APIs,
   no globals, no threads inside core modules unless the spec requires (callers own threads).
7. Before finishing: full local build of your target with `-Wall -Wextra`, tests green, run them 3x for flakiness.

## Commit and PR
- Commit often, message imperative, ending with exactly these two trailer lines (blank line before them):
  `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_014EzMnyGUNDDPMNqjXn92CP`
- Push: `git fetch origin main && git rebase origin/main && git push -u origin feat/<module>`.
  The git proxy allows 2 concurrent operations; on HTTP 429 sleep 10 s and retry once, then continue.
- Open a PR into `main`: `gh pr create --base main --head feat/<module> --title "<module>: <summary>" --body-file <file>`.
  Body: what is implemented, REQ ids covered, test names, Status, needs-from-others. It must end with these lines:
  `🤖 Generated with [Claude Code](https://claude.com/claude-code)` and the session URL on its own line.
  If `gh pr create` fails, leave the branch pushed and report that in your result.
- Do not merge PRs; the orchestrator merges.

## Final report (your last message, <= 120 words)
PR number or branch, test count, what is complete, what is partial or absent and why, anything needed from others.
