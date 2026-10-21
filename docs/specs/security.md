# security (MOD-15), spec

Scope: portable, pure-logic security primitives used by persist, diag, broadcast and the app. No OS calls, no I/O.
Namespace `ss::security`; depends on `base` only. Covers REQ-B03 (secrets never in exports, logs, bundles), REQ-X04
(no default public listener), REQ-S14 (path and archive rejection), spec section 16, test families T-SEC.

## Components
| Header | Purpose |
|---|---|
| `redact.hpp` | `redact`, `redactText`, `redactUrl`, `containsSecret`, `sanitizeForLog`, `truncateUtf8`, `Redactor` (adds literal secret values). Rewrites URL userinfo/query/fragment, stream-key path segments (rtmp, rtmps, rtsp, srt, rtp keep only the app segment; other schemes lose key-looking segments), filesystem usernames (`/Users/x`, `/home/x`, `C:\Users\x`), and secret-like option values (`key=`, `token=`, `passphrase=`, `password: `, `"password": "..."`, `-passphrase value`, `Bearer ...`). Idempotent. |
| `archive.hpp` | `canonicalMemberPath`, `checkArchiveHeader`, `validateArchive`, `ExtractionBudget`. |
| `endpoint.hpp` | `isProtocolAllowed`, `parseEndpoint`, `looksLikeUrlOrUri`, `validateLightingDestination`, `enabledProtocolList`. |
| `secret_store.hpp` | `ISecretStore`, `InMemorySecretStore`, `SecretRef` (`secret:<label>`), `makeRedactorFor(store)`. |
| `worker_limits.hpp` | `WorkerLimits` per worker class, `validate`, `EnforcementReport`, `unenforcedLimits`. |
| `sockets.hpp` | `defaultSocketDeclaration`, `auditSockets`. |

## Rules (what is rejected)
Archive members: empty, over 200 bytes, backslash anywhere, any `:` (drive letters, alternate data streams), control
characters, non-ASCII, leading `/` (absolute and UNC), `.`/`..`/empty/hidden/trailing-dot segments, Windows reserved device names
(CON, PRN, AUX, NUL, COM1-9, LPT1-9), depth over 8, characters outside `[A-Za-z0-9._-]`, symlink/reparse flag reported by the
caller, duplicates after ASCII case folding, a file also used as a directory, directory flag mismatch.
Bombs (checked from headers, before any payload is allocated): entry count (10,000), per-entry size (1 GiB), total (2 GiB),
per-entry and overall compression ratio (100:1 above a 64 KiB floor), zero compressed bytes for non-empty data, u64 overflow.
`ExtractionBudget` then caps actual bytes written to the validated declarations.
Endpoints: local content never allows a network protocol; streams allow `rtmps` and `srt` (plain `rtmp` only via
`EndpointPolicy::allowPlainRtmp`); userinfo, query, fragment, stream keys in the path, bad hosts/ports are rejected, so keys
must be secret references.

## Failure behaviour
`Result`/`Status` with `InvalidArgument`, `PermissionDenied` or `ResourceExhausted`. Error text never echoes the offending input.

## Tests (tests/unit/security, 24 cases)
REQ-S14: hostile path corpus (traversal, drive, UNC, ADS, devices, unicode, controls), case-fold duplicates, symlink flag,
count/size/ratio/overflow bombs, extraction budget, URL smuggling, endpoint allowlist, lighting destination, worker limits.
REQ-B03: redaction corpus (URL, stream key, usernames, 12 option spellings), benign-text no-op, idempotency fuzz (400 seeded
random strings), literal secret scrubbing, log sanitising, secret store. REQ-X04: socket declaration and audit.

## Integration API
```
#include "ss/security/redact.hpp"       // every string that reaches a log, error, journal, export or bundle
std::string safe = ss::security::sanitizeForLog(raw);            // single line, redacted, <= 256 bytes
ss::security::Redactor red = ss::security::makeRedactorFor(secretStore);   // also scrub literal secret values

#include "ss/security/archive.hpp"      // ZIP import: list entry headers only, then validate, then extract
auto ok = ss::security::validateArchive(listing /* vector<ArchiveEntryInfo> from juce::ZipFile */);
ss::security::ExtractionBudget budget(ok.value(), {});            // budget.begin(i); budget.charge(n) per write

#include "ss/security/endpoint.hpp"     // broadcast adapter: parse url + (key from ISecretStore) separately
auto ep = ss::security::parseEndpoint(url, ss::security::Context::Stream);   // ep.value().label() is log-safe

#include "ss/security/secret_store.hpp" // app implements ISecretStore over Credential Manager / Keychain / libsecret
#include "ss/security/worker_limits.hpp" // launcher applies WorkerLimits, fills EnforcementReport, shows unenforcedLimits()
#include "ss/security/sockets.hpp"       // T-SEC port scan: auditSockets(observedListeners) must be empty by default
```

## Status
- Done and unit-tested: everything in the table above (portable logic).
- Redaction is rule-based. It covers the formats listed; it cannot recognise a secret that has no key name, no URL position and
  no registered literal. Defence in depth: `Redactor` literals from the secret store, `scanBundleForSecrets`, and the rule that
  keys never enter the journal or show file in the first place.
- Not done (app layer, cannot be tested in the portable core): OS-backed `ISecretStore`; applying `WorkerLimits` (job objects,
  rlimits, sandbox profiles) and reporting honest `EnforcementReport`; enumerating real sockets for the REQ-X04 port scan; the
  ZIP reader that feeds `validateArchive`. No hardware or OS qualification is claimed.
- Archive member names are restricted to ASCII `[A-Za-z0-9._-]`; this is deliberate (bundle media is content-addressed) and means
  arbitrary international file names cannot appear as members.
