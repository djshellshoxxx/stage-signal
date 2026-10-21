# Module Reference: security

**Purpose.** Security (MOD-15). The rules for what the app accepts from files and users, what gets redacted before it reaches a log, bundle or report, what a stream endpoint may look like, and what listening sockets are allowed. Mostly pure functions. The OS-specific pieces are interfaces the app implements.

**Depends on.** `base`.

## Public headers

| Header | Contents |
|---|---|
| `redact.hpp` | `redact`, `redactText`, `redactUrl`, `containsSecret`, `sanitizeForLog`, `truncateUtf8`, `Redactor` (adds literal secrets), `RedactionResult` |
| `archive.hpp` | `canonicalMemberPath`, `checkArchiveHeader`, `validateArchive`, `ArchiveLimits`, `ArchiveEntryInfo`, `ValidatedEntry`, `ExtractionBudget` |
| `endpoint.hpp` | `parseEndpoint`, `isProtocolAllowed`, `looksLikeUrlOrUri`, `validateLightingDestination`, `enabledProtocolList`, `Endpoint`, `EndpointPolicy` |
| `secret_store.hpp` | `ISecretStore`, `InMemorySecretStore`, `SecretRef` (`secret:<label>`), `makeRedactorFor` |
| `worker_limits.hpp` | `WorkerLimits` per `WorkerClass`, `validate`, `EnforcementReport`, `unenforcedLimits` |
| `sockets.hpp` | `defaultSocketDeclaration`, `auditSockets`, `ObservedSocket`, `DeclaredSocket` |

## Tests

`tests/unit/security`: 24 doctest cases in `test_archive.cpp`, `test_policy.cpp` and `test_redact.cpp`.

## Status (from `docs/specs/security.md`)

- Done and unit tested: the portable logic in the table above.
- Redaction is rule based. It cannot recognise a secret with no key name, no URL position and no registered literal. Defence in depth: literals from the secret store, a bundle scan, and the rule that keys never enter the journal or show file.
- Not done (app layer): the OS-backed `ISecretStore`, applying `WorkerLimits` and reporting a real enforcement result, enumerating real sockets for the port audit, and the zip reader that feeds `validateArchive`.
- Archive member names are limited to ASCII `[A-Za-z0-9._-]`. That is deliberate because bundle media are content addressed.

## Limits

| Item | Value |
|---|---|
| Archive entries | 10,000 |
| Entry size | 1 GiB |
| Total | 2 GiB |
| Compression ratio | 100:1 above a 64 KiB floor |
| Member name | 200 bytes |
| Depth | 8 |
| Log line | 2000 bytes |

No exceptions cross the API. Errors carry no secrets or paths.
