# licenses/ - SPDX inventory expectations

This directory is the human-readable half of the licence inventory. The machine-checked half is
`assets/manifest.json`, verified by `ss::media::verifyManifest()` (`src/media/asset_manifest.h`) and the
`tools/asset_check.cpp` command-line wrapper. Packaging must run the check and fail on any issue (REQ-S15).

## What every entry must provide
Code dependencies (`THIRD_PARTY.md`, one row each, transitive code included):
- name, pinned version (tag AND commit hash before release), vendored path or build recipe;
- SHA-256 of the vendored file/archive;
- an SPDX licence expression (`MIT`, `MIT OR Unlicense`, `LGPL-2.1-or-later WITH ...`; `LicenseRef-...` when no SPDX id exists)
  plus the copyright line;
- where it is used and whether it ships in the product or only in tests/tools;
- patches or build options (LGPL/GPL components also need exact source + relink records, spec §16);
- known-vulnerability triage date at dependency freeze and at release.

Assets (`assets/manifest.json`, schema version 1), per spec §13: `id`, `version`, `type`
(`still|animation|video|font|preset`), `path` (relative, `/`-separated, no `..`), `creator`, `originalUrl`
(may be empty only for `provenanceKind: "original"`), `retrievalDate` (YYYY-MM-DD), `license` (SPDX expression),
`copyright`, `attribution.{text,location}`, `commercialUse` and `modification` (explicit booleans),
`provenanceKind` (`original|derivative|third-party`) with `derivedFrom` for derivatives, `sha256Original` and
`sha256Distributed` (64 lowercase hex; equal for originals), `width`/`height`/`durationMs`/`sizeBytes`,
`flash.{hasFlashing,hasStrobe,maxFlashHz,notes}`, `tiers` (`BASE|STANDARD|ENHANCED|SHOW`).
The top-level `decoders` array lists decoders the package ENABLES (`id`, `enabled`, `containers`, `codecs`); runtime
codec availability is taken from it, never from a file name.

## What the verifier fails on
absent manifest or JSON syntax error; missing/empty required field; invalid SPDX syntax or `NONE`/`NOASSERTION`;
duplicate id or path; unsafe path; file absent; size, SHA-256, image dimension or GIF duration mismatch; original asset
whose two hashes differ; derivative without `derivedFrom`; a file in `assets/` with no manifest entry.

## What it cannot prove
A matching checksum proves byte identity, not ownership or licence validity (spec §13). Item-level rights verification
for any downloaded GIF/preset/font is a human review that must be recorded in `provenanceNotes` before the entry is added.
Original procedural assets and owner-provided imports are the only approved starter content. Imported user files are
never silently redistributed in a shared pack.

## Regenerating the bundled originals
`tests/tools/gen_assets.cpp` (run manually, not at build time; command line in its header) rewrites the three original
assets and `manifest.json`, then self-verifies. Re-run `tools/asset_check.cpp` afterwards.
