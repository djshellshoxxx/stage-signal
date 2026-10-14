# Third-party inventory (MOD-07 scope: media, assets)

Status: working inventory for the media module, not a complete project SPDX document and not legal advice
(spec §16: "No legal approval is claimed"). Other modules append their own rows.
A SHA-256 proves byte identity of the vendored file only, not ownership or licence validity.

## Code dependencies

| Component | Pinned version | Vendored path | SHA-256 of vendored file | SPDX expression | Copyright | Used by |
|---|---|---|---|---|---|---|
| stb_image | v2.30 | `third_party/stb/stb_image.h` | `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3` | `MIT OR Unlicense` | (c) 2017 Sean Barrett | `src/media/stb_impl.cpp` (PNG/JPEG/BMP/GIF only) |
| stb_image_write | v1.16 | `third_party/stb/stb_image_write.h` | `cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05` | `MIT OR Unlicense` | (c) 2017 Sean Barrett | tests and `tests/tools/gen_assets.cpp` only (not shipped) |
| nlohmann/json | 3.11.3 | `third_party/nlohmann/json.hpp` | `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6` | `MIT` | (c) 2013-2023 Niels Lohmann | `src/media/asset_manifest.cpp`, `decoder_registry.cpp` |

Notes
- stb: the file header offers two alternatives, "A - MIT License" and "B - Public Domain (www.unlicense.org)". It is
  NOT MIT-0; the SPDX expression is `MIT OR Unlicense`. Choosing alternative B (public domain) removes the notice
  obligation; if alternative A is relied upon, keep the MIT notice from the header with distributions.
- The upstream commit hashes are not recorded in this repository (files were vendored as plain headers). Before a
  release, pin tag + commit per spec §22 and re-verify the hashes above.
- stb_image v2.30 defect (found by ASan in this work): `stbi_load_gif_from_memory()` computes `two_back = out - 2*stride`
  and reads before the frame buffer when a GIF uses disposal method 3 after its second frame. StageSignal does NOT call
  that function; `src/media/stb_impl.cpp` drives stb's single-frame decoder and performs disposal itself.
  The vendored header is unmodified. stb also returns the frames decoded so far for truncated GIFs; the wrapper rejects them.
- Build options that shrink the attack surface: `STBI_ONLY_{PNG,JPEG,BMP,GIF}`, `STBI_NO_STDIO`, `STBI_NO_LINEAR`,
  `STBI_NO_HDR`, `STBI_MAX_DIMENSIONS=8192`.
- No video decoder library is bundled. `assets/manifest.json` therefore lists no enabled decoders; video import reports
  `UnsupportedCapability` until a platform adapter registers a decoder AND the manifest enables it (spec §13).
  Codec patent obligations are a separate decision (spec §16).

## Bundled assets (all in `assets/`, described in `assets/manifest.json`)

| Asset ID | File | Creator | SPDX | Provenance |
|---|---|---|---|---|
| `cdl.testcard.320x240` | `testcard_320x240.png` | Circuit Drift Labs | `CC0-1.0` | original, procedural (`tests/tools/gen_assets.cpp`) |
| `cdl.placeholder.missing-media.320x240` | `missing_media_320x240.png` | Circuit Drift Labs | `CC0-1.0` | original, procedural (same pixels as `media/placeholder.cpp`) |
| `cdl.loop.dot.64x48` | `loop_dot_64x48.gif` | Circuit Drift Labs | `CC0-1.0` | original, procedural |

The CC0-1.0 dedication and the "Circuit Drift Labs" creator/copyright statement are the project owner's declaration as
supplied for this work; the owner confirms them at M0 (spec §16 "Owner selects StageSignal license at M0").
No external animation pack, preset, font or GIF collection is approved or bundled (spec §13).
