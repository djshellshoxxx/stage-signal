# security (MOD-15), spec

Scope: portable, pure-logic security primitives used by persist, diag, broadcast and the app. No OS calls, no I/O.
Namespace `ss::security`. Depends on `base` only. Covers REQ-B03 (secrets never in exports/logs/bundles), REQ-X04
(no default public listener), REQ-S14 (path/archive rejection), spec section 16, tests T-SEC.

## Components
| Header | Purpose |
|---|---|
| `redact.hpp` | `redactText`, `redactUrl`, `scanSecrets`, `Redactor` (extra literal secrets). Strips URL userinfo, query, fragment, stream-key path segments, filesystem usernames, secret-like option values (`key=`, `token=`, `passphrase=`, `password=`, `Bearer ...`). Idempotent. |
| `archive.hpp` | `canonicalMemberPath`, `validateArchive` (entry list), `ExtractionBudget`. Rejects absolute, drive, UNC, `..`, any backslash, ADS colons, control chars, reserved device names, non-allowlisted characters, reparse/symlink flag, case-fold duplicates, count/size/ratio bombs. Count and declared totals are checked before any per-entry work, so callers allocate nothing for a rejected archive. |
| `endpoint.hpp` | Protocol allowlist: `Context::LocalContent` allows no network protocol; `Context::Stream` allows `rtmps` and `srt` only (plain `rtmp` opt-in). `parseEndpoint` rejects userinfo, query secrets, bad hosts and ports. `validateLightingDestination` (IPv4 literal). `enabledProtocolList()` for the dependency inventory. |
| `secret_store.hpp` | `ISecretStore` (put/get/remove/exists by `SecretRef`), `InMemorySecretStore` fake, `isValidSecretRef`. The OS-backed store (Credential Manager / Keychain / libsecret) is app layer. |
| `worker_limits.hpp` | `WorkerLimits` descriptor (memory, wall time, CPU, process count, output bytes, network off, private temp dir only) with defaults per worker class and `WorkerLimitReport` so unenforced limits are reported, not assumed. |
| `sockets.hpp` | `defaultSocketDeclaration()` (REQ-X04): zero inbound listeners; only opt-in outbound UDP lighting. `auditSockets(observed)` for the port-scan test. |

## Failure behaviour
All functions return `ss::Result`/`Status` with `ErrorCategory::InvalidArgument` / `PermissionDenied` / `ResourceExhausted`.
Error detail never echoes the offending input beyond a redacted, length-capped excerpt.

## Tests (tests/unit/security)
REQ-S14 path/archive attacks (hostile corpus), REQ-B03 redaction corpus + idempotency + fuzz-ish random strings,
REQ-X04 socket declaration, endpoint allowlist, secret store, worker limits.

## Status
See bottom of file (updated at completion).
