# Security Model

Spec: `docs/specs/security.md`, plus the parts of `app-adapters.md` and `persist.md` that touch secrets and files. StageSignal is offline first. Its security work is about what it accepts from files and users, what it writes into logs and bundles, and what it exposes on the network.

## What the core does

| Component | What it does |
|---|---|
| `redact` | Rewrites URL userinfo, query and fragment. Removes stream-key path segments. Adds literal secret values from the secret store. Makes log lines one line and at most 2000 bytes. |
| `archive` | Checks zip member names and headers before any payload is read. Enforces entry counts and size and compression-ratio limits. Caps actual bytes written to the declared sizes. |
| `endpoint` | Parses and validates stream and lighting destinations. Keys must be secret references, never inline. |
| `secret_store` | Interface the app implements over Windows Credential Manager, the macOS Keychain or libsecret. The core ships an in-memory store for tests. |
| `worker_limits` | Describes limits per worker class and produces an enforcement report that says what was really enforced. |
| `sockets` | Audits the listening sockets seen at idle. The default is none. |

## Archive rules (zip import)

Rejected member names:

- empty, longer than 200 bytes, or containing a backslash, a colon, control characters or non-ASCII
- starting with `/` (absolute paths and UNC)
- `.`, `..`, empty, hidden or trailing-dot segments
- Windows reserved device names (CON, PRN, AUX, NUL, COM1 to COM9, LPT1 to LPT9)
- depth over 8, or characters outside `[A-Za-z0-9._-]`
- symlinks or reparse points, duplicates after case folding, a file used as a directory

Limits, checked from headers before allocation: 10,000 entries, 1 GiB per entry, 2 GiB total, and a 100:1 compression ratio above a 64 KiB floor.

## Endpoint rules

- Local content never allows a network protocol.
- Streams allow `rtmps` and `srt`. Plain `rtmp` needs an explicit policy and is labelled as unencrypted.
- URLs with userinfo, query, fragment or a stream key in the path are rejected.
- Lighting destinations must be IPv4.

## Secrets

- Stream keys are never written to the journal, a show file, a log or a support bundle.
- The app stores keys in the OS store and keeps only a reference (`secret:<label>`) in settings.
- The UI shows "saved", never the key.

Known exposure (P-10): in version 1 the stream key reaches the FFmpeg command line, which other local users can read in a process list. FFmpeg's file-based option did not apply to the muxer option we need (tested, see `research-deferred.md`). Treat broadcast on shared machines as exposed until this is fixed.

## Support bundle

Allowed categories: app info, system summary, event journal, logs and capabilities by default. Show structure, redacted settings and the show report are optional.

Rejected by category or content: raw audio, camera frames, key material, full media and crash dumps. Also rejected: files with extensions such as wav, mp3, mp4, png, jpg, dmp, pem, key and zip, and content with NUL bytes, invalid UTF-8, a PEM header or media magic numbers.

Limits: 64 items, 8 MiB per item, 32 MiB total. Every item is redacted, then scanned again. If the scan does not converge, the bundle fails closed. The operator sees a preview before composing.

## Show Report

No secrets, no absolute paths, no raw audio or video. CSV cells that start with `=`, `+`, `-`, `@`, tab or CR get a leading apostrophe. HTML is escaped everywhere, including inside SVG text, and the page has a CSP that blocks any external resource.

## Network surface

- No listening sockets by default. The Show Doctor check D-NET-1 flags any that appear.
- No update check, telemetry, login or asset fetch exists in the code (APP-SH-11).
- Art-Net and sACN send packets out. They do not listen for input.

## Process limits

The app is supposed to apply `WorkerLimits` (job objects, rlimits, sandbox profiles) and report what it could not enforce. That app code is not written. Until it is, the limits are declared, not enforced.

## Known weak spots

- Redaction is rule based. It can miss a secret with no key name, no URL position and no registered literal. Defence in depth is the reason the rule "keys never enter the journal or show file" exists.
- Member-name rules deliberately block international file names inside bundles, because media are stored by hash.
- The security review has not been done by an outside party. No audit has been commissioned.
- Local Network permission on macOS and firewall prompts on Windows are OS decisions, not StageSignal controls (P-09).

## Reporting a problem

Use the repository's issue tracker. Do not attach secrets, keys or full show files with media. A support bundle is the safest attachment, after you have checked its preview.
