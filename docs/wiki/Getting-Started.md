# Getting Started

**Status:** Beta packages are produced by the `beta.yml` workflow (see [Testing and CI](Testing-and-CI)). As of this wiki build the repository has no published release. The current `main` app is a placeholder window, so the steps below are what a beta will look like, not something you can run today.

## What you get

Each beta ships two formats for four targets, plus `SHA256SUMS.txt`.

| Target | Standalone | CLAP plugin |
|---|---|---|
| Windows x64 | `StageSignal-<tag>-windows-x64-standalone.zip` (`StageSignal.exe`) | `StageSignal-<tag>-windows-x64-clap.zip` (`StageSignal.clap`) |
| macOS arm64 (Apple silicon) | `...-macos-arm64-standalone.zip` (`StageSignal.app`) | `...-macos-arm64-clap.zip` (`StageSignal.clap`) |
| macOS x86_64 (Intel) | `...-macos-x86_64-standalone.zip` | `...-macos-x86_64-clap.zip` |
| Linux x64 | `...-linux-x64-standalone.tar.gz` | `...-linux-x64-clap.tar.gz` |

Check the download first:

```sh
# Verify the download against the published checksums
sha256sum -c SHA256SUMS.txt --ignore-missing
```

## Windows

1. Unzip the standalone or the CLAP package.
2. Run `StageSignal.exe`. SmartScreen may warn because the beta is unsigned. Choose **More info**, then **Run anyway**.
3. For the plugin, copy `StageSignal.clap` into the standard CLAP folder, `%COMMONPROGRAMFILES%\CLAP\`, then rescan plugins in your host.

## macOS

The beta is ad-hoc signed and not notarised. Gatekeeper blocks a plain double click.

```sh
# Clear the quarantine flag once, then open the app
xattr -dr com.apple.quarantine StageSignal.app
open StageSignal.app
```

For the plugin, use `xattr -dr com.apple.quarantine StageSignal.clap` and copy it to `~/Library/Audio/Plug-Ins/CLAP/`. You can also use right-click, then **Open**, instead of `xattr`.

macOS may ask for Local Network access. Lighting output to LAN addresses needs it. If you deny it, Art-Net and sACN sends can fail silently on some versions (see [Lighting and DMX](Lighting-and-DMX)).

## Linux

```sh
# Unpack the standalone build and run it
tar -xzf StageSignal-<tag>-linux-x64-standalone.tar.gz
./StageSignal
```

The standalone needs ALSA and X11 at runtime. The plugin goes to `~/.clap/`:

```sh
# Install the CLAP plugin for the current user
mkdir -p ~/.clap
tar -xzf StageSignal-<tag>-linux-x64-clap.tar.gz -C ~/.clap
```

There is no Wayland window support for the plugin. Hosts on Wayland run through XWayland (see [Known Limitations](Known-Limitations)).

## Optional: FFmpeg

Video recording and broadcast use an external FFmpeg. It is never bundled. Put `ffmpeg` on your `PATH`, or next to the app executable. If it is missing, those features turn off and say so. The Show Doctor reports whether it was found (`D-REC-2`).

## Before your first show

1. Read [Safety and Blackout](Safety-and-Blackout).
2. Run the Show Doctor (see [Show Doctor and Troubleshooting](Show-Doctor-and-Troubleshooting)).
3. Test blackout (F8) on an empty rig before you rely on it.
4. Keep lighting disarmed until you have checked the patch.

## Planned

- First-run setup wizard (`APP-UI` section 9): not built.
- Automatic updates: deliberately not part of the design (offline first).
