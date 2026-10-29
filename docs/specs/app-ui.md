# app-ui: operator UI, theme, wizard, Show Doctor, Command Palette, Undo/A-B, Show Report, text panel

Status: spec v0.2-draft, 20 Oct 2026. Layer: `app/Source/UI`. Baseline: spec v0.1 section 11 (UI), 13 (assets), 19 (help), addendum DEC-13, `docs/ARCHITECTURE.md`, `core/base/include/ss/base/actions.hpp` (action registry), `core/model/include/ss/model/show_model.hpp`.
Nothing is implemented. Colour numbers below were computed with a script (WCAG relative-luminance formula) and are proposals until checked against the owner's Circuit Drift Labs (CDL) design system; any token the CDL system defines wins.

## 1. Scope and requirement ids

In: the 800x600 operator window, every tab, status bar, bottom strip, keyboard map, focus rules, compact layout, theme and accessibility, first-run wizard, Show Doctor panel, Command Palette, Undo/Redo and A/B snapshots (controls only; logic is in `show`), Show Report dialog (logic in `diag`), text Draft/Preview/Publish panel.
Out: audience windows' drawing (render core), adapters (`app-adapters.md`), threads (`app-shell.md`).

| ID | Statement | Spec REQ |
|---|---|---|
| APP-UI-01 | Operator window is laid out for 800x600 logical px: 48 px top bar, 60 px bottom strip, tab area in between; no required live control needs scrolling | REQ-D10, spec 11 |
| APP-UI-02 | Below 800x600 logical px (either axis) the compact layout engages; minimum supported 640x480; below that a banner states the requirement and the page scrolls; fonts never go under 14 px effective | REQ-D10, spec 11 |
| APP-UI-03 | Every state is conveyed by text or icon shape in addition to colour | REQ-X02 |
| APP-UI-04 | Every interactive control is keyboard reachable with a visible focus ring (3:1 against both neighbours); tab order is defined (section 7) | REQ-X02, REQ-D10 |
| APP-UI-05 | Scene pad keys 1 to 8 never act while a text input has focus; F-keys still do | spec 11 |
| APP-UI-06 | The Live tab holds the 8 scene pads, bank navigation, output target (always visible), tempo/key, looks, effects, Energy Arc, snapshots; emergency actions are in the bottom strip on every tab | REQ-D08, REQ-D11 |
| APP-UI-07 | Blackout controls are latched buttons with a visible latched state; restoring is an explicit action | REQ-D08 |
| APP-UI-08 | Contrast: body text 4.5:1, large text and control boundaries 3:1; live control height at least 32 px (bottom strip 44 px) | spec 11, WCAG 1.4.3 / 1.4.11 |
| APP-UI-09 | Custom components expose JUCE `AccessibilityHandler`s with role, name, value and state; status changes are announced | REQ-X02 |
| APP-UI-10 | Night theme (default) and High-contrast theme; Appearance toggle "Clean mode" removes the hand-made touches | spec 11 |
| APP-UI-11 | UI copy contains no em dashes and none of the banned phrases; a lint test scans all strings | owner direction |
| APP-UI-12 | Setup wizard (UJ-01) completes by keyboard at 800x600; nothing transmits; DMX and broadcast end the wizard Off | REQ-D10, UJ-01 |
| APP-UI-13 | Show Doctor runs read-only (no datagram, no device open beyond enumeration, no camera start) and reports Blocker / Warning / Info / Pass | NEW-06, REQ-L03 |
| APP-UI-14 | Command Palette (Ctrl+K) lists every registered action valid in context, runs it through IF-02, and cannot bypass confirmations or safety rules | NEW-07 |
| APP-UI-15 | Undo/Redo covers show-document edits only; A/B snapshots store and recall look state with a crossfade | NEW-03 |
| APP-UI-16 | Show Report dialog exports HTML and CSV with an explicit category preview; no secrets, no absolute paths | NEW-05, REQ-X05 |
| APP-UI-17 | Text: Draft, Preview and Publish are separate states; only Publish changes audience outputs; Publish names its target | REQ-S05 |
| APP-UI-18 | Audience windows never show operator panels, diagnostics, stream secrets or file paths unless diagnostics overlay is explicitly enabled per output | REQ-D05 |
| APP-UI-19 | Preview pane runs at 5 to 15 FPS and shows a "Preview" label; it never transmits and never is the program output | REQ-L03, spec 11 |
| APP-UI-20 | Per-control state table (section 5) is complete: each control lists enabled, disabled-with-reason, busy and error behaviour | AUD-11 |
| APP-UI-21 | The UI thread never blocks on file, network, decode or device calls; long operations show a busy state and are cancellable | spec 8 |
| APP-UI-22 | Host-plugin mode: all emergency actions and the palette are also reachable with the mouse; keys the host swallows are listed in the Help keyboard map | DEC-11 |

## 2. Window model

- Standalone: one operator `juce::DocumentWindow`-derived `OperatorWindow`, resizable, min 640x480, default 800x600 client; native title bar; position and size remembered per display identity (not index).
- CLAP: embedded editor with the same root `Component`; default 800x600, host resize honoured, compact layout rules apply.
- Root component `OperatorRoot` owns `StatusBar` (48), `TabHost` (rest) and `PerformanceStrip` (60). Heights are logical pixels (JUCE scales to the display).
- Audience windows are separate and covered by app-adapters 4. A keyboard shortcut or button in the operator UI can never create an audience window on the display that currently holds the operator window.

Region geometry at exactly 800x600 client:

| Region | Rect (x, y, w, h) |
|---|---|
| Top status bar | 0, 0, 800, 48 |
| Tab bar | 0, 48, 800, 36 |
| Page area | 0, 84, 800, 456 |
| Bottom performance strip | 0, 540, 800, 60 |

### Compact layout rule

Trigger: logical client width < 800 or height < 600, evaluated on every resize and display-scale change (e.g. a 1366x768 laptop at 150% scale has 910x512 logical px, so it is compact because of height).
Compact differences (data-driven, same components):
1. Status bar chips drop their text label and show icon + value; the full label stays as tooltip and accessible name.
2. Tab bar shows icons with short names (Live, Setup, Media, MIDI, Rec, Diag, Help); the 36 px height stays.
3. Live tab becomes one column: pads first, then tempo/key, then looks; the preview collapses to a 160x120 thumbnail that expands to the full preview on click.
4. Pages scroll vertically; the bottom strip never scrolls.
5. Below 640x480 a persistent banner reads "Window too small. StageSignal needs at least 640 by 480." and nothing else changes. Fonts remain at least 14 px; we do not shrink text to fit.
6. Compact is reported in Diagnostics (supported-size requirement text) as spec 11 asks.

## 3. Theme and identity (Circuit Drift Labs)

### 3.1 Night theme tokens (proposed)

| Token | Value | Use | Contrast (computed) |
|---|---|---|---|
| `bg.night` | `#04060B` | window background (given by the owner) | n/a |
| `bg.panel` | `#0B0F18` | cards, strips | text 16.5:1 |
| `bg.raised` | `#131A27` | buttons, inputs | text 15.0:1 |
| `bg.sunken` | `#020306` | meters, preview frame | n/a |
| `text.primary` | `#E9EEF7` | body | 17.4:1 on night |
| `text.secondary` | `#A3AEC4` | labels | 9.1:1 on night, 7.8:1 on raised |
| `text.muted` | `#7F8BA3` | hints, disabled text on night only | 5.9:1 on night, 5.1:1 on raised |
| `accent.spray` | `#FF4FA8` (magenta) | primary actions, active scene, focus accent | 6.7:1 on night, 5.8:1 on raised |
| `accent.rain` | `#3FE0FF` (cyan) | live data: meters, tempo, links | 12.9:1 on night |
| `state.ok` | `#4ADE80` | OK, armed-safe | 11.6:1 |
| `state.warn` | `#FBBF24` | warning | 12.1:1 |
| `state.danger` | `#FF5C6C` | fault, blackout latched | 6.8:1 |
| `line.control` | `#6C7A9A` | control outlines | 4.7:1 on night, 4.1:1 on raised (needs 3:1) |
| `line.divider` | `#44506B` | decorative dividers only (2.2 to 2.5:1; never the only cue for a control) |
| `focus.ring` | `#FFFFFF` 2 px outer + 1 px `bg.night` gap | focus | 20:1 |
| Text on `accent.spray` fill | `#04060B` | 6.7:1 |
| Text on `accent.rain` fill | `#04060B` | 12.9:1 |
| Text on danger fill `#B3122B` | `#FFFFFF` | 6.9:1 |

"Spray" is the magenta used like spray paint: bold fills and a soft 6 px glow on the active scene pad. "Rain" is the cyan used as thin lines and fine data. Rule: spray means "you chose this", rain means "the machine reports this". Glows are decoration only and never change contrast of text.

### 3.2 High-contrast theme

Black `#000000` background, white `#FFFFFF` text (21:1), 3 px focus ring, all outlines `#FFFFFF`, accents replaced by white/yellow `#FFFF00` and cyan `#00FFFF`, no glow, no imperfections, 7:1 minimum for all text. JUCE exposes dark mode (`Desktop::isDarkModeActive`, `Desktop::DarkModeSettingListener`) but I found no high-contrast detection in `juce_gui_basics`, so this theme is a manual setting (Setup > Appearance); automatic OS high-contrast following is **not** provided (honest limit).

### 3.3 Type

- Display (product name, tab titles, section headers only): a stencil face, bundled under the SIL OFL. Candidates present in the Google Fonts repository under OFL (license files fetched): Big Shoulders Stencil, Allerta Stencil, Saira Stencil One. Default pick: Big Shoulders Stencil; swap for the face the CDL system specifies. The font and its `OFL.txt` go in `assets/` with a manifest entry (REQ-S15).
- Body: the OS UI font via `juce::Font::getDefaultSansSerifFontName()`. Numerals for BPM, time and meters: default monospaced face so digits do not jump.
- Sizes (logical px): body 14, small 14 (no smaller; spec 11 asks for 14 px effective minimum), section header 16 stencil, tab title 15 stencil, status chip value 15.
- Title font is used for at most a short word or two; never for paragraphs.

### 3.4 Tiny deliberate hand-made imperfections

Allowed list, all drawn from a deterministic RNG seeded by a hash of the control's id (`ss::Rng`), so every launch looks the same and golden screenshots are stable:
1. The stencil title lockup is rotated 0.4 degrees and sits 1 px off the baseline grid.
2. The active tab underline is a hand-drawn stroke: 6 control points with up to 0.8 px jitter.
3. One meter in the status bar has a tick mark 1 px out of line with its siblings.
4. The "spray" glow on the active pad is offset 2 px down-right from the pad outline, like a misregistered print.
5. A single divider line in each page has a 3 px gap.
6. The Help tab version line uses a slightly different letter spacing on one character.
Rules: never on emergency controls, never changes hit areas, text contrast, focus order or layout size; max 6 touches on screen at once; not in High-contrast; switchable with Setup > Appearance > "Clean mode" (also the default in compact layout). Test: hit-rect and layout snapshot identical with and without them.

### 3.5 Copy rules ("no AI-sounding copy, no em dashes")

- No em dash (U+2014) and no "--" as punctuation in any UI string, tooltip, dialog or Help text. Use a colon, a comma, a period or a plain hyphen between words. En dash only in numeric ranges is also avoided: write "1 to 8".
- Sentence case, short, concrete, second person only when giving an instruction. Say what happened and what to do.
- Banned (case-insensitive) in strings: seamless, effortless, unlock, elevate, empower, leverage, supercharge, "powerful", "robust", "delve", "dive into", "let's", "oops", "whoops", "we're sorry", "magic", "game-changing", "unleash", "in today's", "journey", "tapestry", "it's worth noting", "please note that", exclamation marks except in the literal sample show title.
- Error text: cause, current state, next step. Example: "Audio device not found. Show keeps running without analysis. Pick a device in Setup." No raw error codes, no file paths, no secrets.
- Voice: plain and a bit dry. Examples: "Strobe is locked off." "2 outputs, 1 unmatched." "Armed. Lights are live."
- Test `ui: copy lint` loads the string table (`app/Source/UI/strings.cpp`, single place for all literals) and fails on any banned token or U+2014/U+2013.

## 4. Status bar (48 px) and bottom strip (60 px)

### 4.1 Status bar chips (left to right, widths at 800 px)

| Chip | Width | Text and icon per state | A11y name example |
|---|---|---|---|
| Show | 118 | stencil "STAGESIGNAL" + show name, "*" dot when unsaved | "Show: Friday Set, unsaved changes" |
| Audio | 150 | `Audio: <device>` + in/out meter; states: Ok, No device (X icon), Lost (broken-link icon), Clipping ("CLIP" pill, rain stays), Silence ("No signal" after 5 s) | "Audio input Scarlett 2i2, level minus 14 dB" |
| Mode | 84 | Auto / Programmed / Hybrid; suffix "Paused" with pause icon; "Override" in Hybrid | "Mode Hybrid, automation paused" |
| Tempo | 112 | `128.0 BPM` + label `Est`, `Tap`, `Manual`, `Host` + confidence bar + age; "BPM unknown" when none | "Tempo 128 BPM, tapped" |
| Key | 92 | `A minor` + label; "Key unknown" | "Key A minor, estimated, low confidence" |
| Camera | 62 | Off, Preview, Published (eye icon), Lost (slash icon) | "Camera published" |
| DMX | 62 | Off, Dry run, Armed (shield icon, spray fill), Fault | "Lighting armed" |
| REC | 70 | Idle, `00:12:34` red dot while recording, Failed | "Recording 12 minutes 34 seconds" |
| LIVE | 50 | Off, Connecting, Live, Retry n, Error | "Broadcast off" |

Sum: 118+150+84+112+92+62+62+70+50 = 800. Chips are focusable (read-only) so a screen reader reaches them; Enter on a chip jumps to its tab (Audio -> Setup, REC -> Record, etc.). Labels like "Est" are never dropped even in compact layout (the full word stays in the accessible name).

### 4.2 Bottom strip (60 px, 44 px buttons, 8 px padding)

Order: [Visual Blackout] [Lighting Blackout] [EMERGENCY BLACKOUT] [Restore] | [Pause] [Resume] [Next] | Brightness slider | Intensity slider.

| Control | Action id | Key | States |
|---|---|---|---|
| Visual Blackout | `VisualBlackout` | Ctrl+F8 | latched shows "Visual blackout ON" + danger outline; press again does nothing (restore is explicit) |
| Lighting Blackout | `LightingBlackout` | Ctrl+Shift+F8 | same; shows "no lights" when lighting is Off (still latches, so a later Arm starts blacked out) |
| EMERGENCY BLACKOUT | `EmergencyBlackout` | F8 | affects visual and lighting channels only, never audio recording; wider, danger fill |
| Restore | `RestoreBlackout` | Shift+F8 | enabled only when something is latched; label lists what it restores: "Restore visual and lighting" |
| Pause / Resume | `Pause` / `Resume` | F6 / F7 | Pause disabled when already paused; Resume offers "Continue cue" or "Next cue" on press when in Programmed mode (REQ-S04) via a small two-button popover |
| Next | `NextScene` | F5 | in Programmed mode shows "Next cue" |
| Brightness | `Brightness` | Alt+Up/Down when focused (slider native keys: arrows) | 0 to 100 |
| Intensity | `Intensity` | same | 0 to 100; label says "Scene intensity" to avoid confusion with lighting intensity |

All strip buttons stay enabled during modal dialogs where the OS allows; dialogs are non-modal panels inside the window wherever possible so F8 always reaches the dispatcher. Keyboard shortcuts are in-app only (not OS-global hotkeys, spec 11).

## 5. Tabs: controls and states

Conventions for every table: Enabled = normal; Disabled shows the reason as a tooltip and as accessible description; Busy shows a spinner and "Working" text; Error shows sanitised text next to the control.

### 5.1 Live tab (page 800x456)

| Block | Rect | Contents and states |
|---|---|---|
| Target selector | 8, 8, 344x32 | combo "Target: All visual outputs / Main / Side..." always visible; value feeds every action that has `target`; disabled outputs listed greyed with "Disconnected" |
| Preview | 8, 44, 344x258 | 4:3 low-rate preview of the selected target, label "Preview", overlay "BLACKOUT" when latched; if the output is a camera-containing scene, a small "camera not published" note |
| Tempo | 8, 310, 344x44 | [Tap] (`TapTempo`, key T) [BPM spinner 40 to 240, `SetBpm`] [Release] (`ReleaseBpmOverride`, enabled only if tap/manual active) [Key combo, `SetKey`, and "Auto" entry = `ReleaseKeyOverride`] |
| Energy Arc | 8, 362, 344x40 | 0 to 1 bar (rain), phase word "Steady / Build / Drop" with icon shape, on/off toggle (setting stored in show) |
| Mode | 360, 8, 432x32 | radio Auto / Programmed / Hybrid (`SetMode`); in Programmed or Hybrid: "Cue 3 of 12: Verse 2" with [Prev] [Next] [Go to...] |
| Scene pads | 360, 48, 432x136 | 4 x 2 grid, each 102x64, 8 gap; shows number 1 to 8, name (2 lines max), small thumbnail, state: Idle, Active (spray fill), Queued (rain outline + "next"), Unavailable (missing media, tooltip), "FLASH" badge on strobe scenes, and when strobe is locked a lock icon and the pad is disabled; pressing a pad in Auto mode switches to Hybrid override with a toast "Manual override. Auto paused. Resume to continue." |
| Bank row | 360, 192, 432x32 | [<] "Bank 1: name" [>] with dots; `BankPrev`/`BankNext` |
| Looks | 360, 232, 432x92 | sliders Speed (`Speed`), Hue (`ColorHue`), Saturation (`ColorSaturation`) |
| Effects | 360, 332, 432x68 | row "Prism Mirror": toggle + amount slider (`EffectPrismMirror`) + segments spinner 2 to 16; row "Bloom Pulse": toggle + amount slider (`EffectBloomPulse`); amount 0 = off |
| Edit/snapshots | 360, 408, 432x40 | [Undo] [Redo] ; [A store] [A recall] [B store] [B recall] with fade time field; indicator "A" / "B" / "A*" |

Disabled rules: Tap disabled never (always allowed); pads disabled while text input has focus is NOT a rule (keys only); `ArmLighting` is not on this tab.

### 5.2 Setup tab (scrolling page; sections collapse; first section "Run setup wizard")

| Section | Controls | Notes |
|---|---|---|
| Audio | device type combo, device combo (stable identity shown as "name (type)"), input route (Left, Right, Both, Mono mix), sample rate combo (44.1k, 48k only; other rates greyed "not supported"), buffer size combo, input meter + clip pill, input trim −24 to +12 dB, "Refresh devices" | device change during Recording is disabled with a reason |
| Displays | table of connected displays: stable name, size, scale, position, current role combo (Controls, Audience, Duplicate of..., Independent, Unused), [Identify] (shows a large number on each display for 5 s, operator triggered), [Test pattern] (border/grid for VGA check) | roles saved by identity; ambiguous identical monitors show "Cannot tell these apart. Pick which is which." and remain idle until confirmed |
| Outputs | list of logical outputs with label, route, overlay toggles (BPM/key, text, branding), diagnostics overlay (off by default with an extra confirm) | |
| Safety | "Disable strobing" toggle (default ON for new shows), "Lock quality" toggle, "Confirm before leaving show running" | strobe lock disables FLASH scenes and mappings and fixture strobe channels (REQ-S12) |
| Appearance | theme (Night / High contrast), Clean mode, UI scale note | |
| Show files | New, Open, Save (Ctrl+S), Save a copy, Export portable (.stagesignal), Import, Recent, autosave age, backups list | import errors show the sanitised validator message and "Your current show is unchanged." |
| Benchmark | Run quick benchmark (15 s warm-up + 60 s), history table, admitted features, recommended tier, Lock result | benchmark never opens broadcast or lighting transports (REQ-P02) |
| Privacy | opt-ins: Crowd mic moments, Silhouettes (hidden until the feature exists), each with plain-language text | default off |

### 5.3 Media/Text tab

Left: media list (thumbnail 48x36, name, type, size, flash badge, status Ok / Missing / Invalid) with [Import...], [Remove], properties (loop, speed 0.25 to 4 GIF / 0.25 to 2 video, crop). Import runs in the media worker; the row shows Busy; invalid files show "Could not read this file. Original scene kept." (REQ-S08).
Right: text panel, section 11.

### 5.4 MIDI/Lighting tab

MIDI: input and output port lists (name, state Connected / Lost), [Learn] flow, mapping table (device, channel, type, number, range, encoder mode, action, [Edit] [Delete]), clock out toggle (opt-in, tempo source combo), SysEx toggle (off; limit text "up to 1024 bytes"), feedback alarms list (suspended route + reason + [Re-enable]).
Learn flow states: Idle -> "Waiting for a MIDI message. Move a control. [Cancel]" -> captured (shows device/channel/type/number) -> action combo -> [Save] / [Cancel]. Esc cancels. Lost device mid-learn: "Device disconnected. Learn cancelled."
Lighting: protocol (Art-Net / sACN), network interface combo (shows address; no default pick when more than one), destination (unicast IP; broadcast checkbox with warning "Sends to every device on this network"), universe (Art-Net shows "Net/SubNet/Universe" and the 15-bit Port-Address; sACN 1 to 63999), rate 25 default (max 40), sACN priority 100, fixture patch table (profile, universe, start, footprint, conflict marker), profile import, [Dry run] toggle with channel monitor, [Arm] / [Disarm], Blackout test.
Arm confirmation (inline panel, not a blocking dialog): "Arm lighting to <ip> on <interface>? Universe 1, 12 fixtures. Lights will respond." [Arm] [Cancel]. After any fault or reconnect the chip shows Fault and Arm is required again (REQ-L05). Unsupported USB-DMX adapters are listed as "Not supported" with no Arm option; text never says "compatible".

### 5.5 Record/Broadcast tab

Recording column: source (program output combo), audio bus label (Processed), format profile combo, folder (path shown as display name + free space), dynamics panel (trim, compressor: threshold, ratio, attack, release, knee, makeup; limiter: ceiling, lookahead, release; bypass toggle; gain-reduction meter; input vs output meter with "possible input clipping" wording), [Start recording] [Stop recording] (separate buttons, spec 11), [Add marker] with text, elapsed, size, remaining estimate with ± (spec 12), segment list, state chip (Idle, Preparing, Recording, Stopping, Completed, Failed, Recoverable). Start failure text states the cause before claiming Recording (REQ-R06): "Cannot record: folder is not writable. Pick another folder."
Broadcast column: endpoint (scheme combo RTMPS / RTMP (unencrypted label) / SRT / RTP LAN), host, port, path, stream key field (masked; after save shows "Key saved in system keychain" and a [Replace] button; never shown again; never in logs), video (resolution, fps, bitrate, keyframe interval), audio (rate, bitrate), reconnect (on, with countdown), publication summary box before Start ("Audience output: Main. Audio: processed bus. Camera: not published."), [Start broadcast] [Stop broadcast], state (Off, Preparing, Connecting, Live, Reconnecting 8 s, Stopping, Error with reason). Stop cancels reconnect within 1 s. Local recording continues independent of broadcast errors.

### 5.6 Diagnostics tab

Show Doctor (section 9), live status (thread heartbeats, queue fill, xruns, dropped frames by type, CPU/memory, adaptation step), event journal viewer (filter by category, search), support bundle export (category preview, spec 16), Show Report button (section 12), log level, build id.

### 5.7 Help tab

Topic list (the 14 topics in spec 19: display roles, gain staging, BPM/key confidence, scene modes, live text, camera publication, MIDI learn and clock, lighting arm/blackout/timeouts, benchmark interpretation, recorder recovery, broadcast protocols/credentials, portable export, keyboard map, sanitised diagnostics), first-show walkthrough (6 screens), troubleshooting per device class, keyboard map (generated from the action registry, so it cannot drift), host-key conflicts note in plugin mode, About: version, build id, third-party notices, **source offer** (StageSignal is AGPL-3.0-or-later per DEC-05; the About page must give the repository URL so users can get the corresponding source), font and asset licences.

## 6. Keyboard map

| Key | Action | Notes |
|---|---|---|
| F1 | Help tab | |
| F5 | `NextScene` | |
| F6 / F7 | `Pause` / `Resume` | |
| F8 | `EmergencyBlackout` | works from any focus, including text fields |
| Shift+F8 | `RestoreBlackout` | |
| Ctrl+F8 / Ctrl+Shift+F8 | `VisualBlackout` / `LightingBlackout` | |
| 1 to 8 | `SceneSelect` slot | disabled while a text input has focus |
| T | `TapTempo` | disabled while a text input has focus |
| Ctrl+S | `SaveShow` | |
| Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z | `Undo` / `Redo` | in a text field these act on the text first |
| F2 / Shift+F2 | recall A / store A | |
| F3 / Shift+F3 | recall B / store B | |
| Ctrl+K | `OpenCommandPalette` | |
| Ctrl+1 to Ctrl+7 | switch to tab 1 to 7 | |
| Ctrl+Tab / Ctrl+Shift+Tab | next/previous tab | |
| Ctrl+F6 | cycle focus between status bar, tabs, page, strip | |
| Tab / Shift+Tab | next/previous control | |
| Esc | close palette, cancel learn/wizard step, close popover | never stops recording or broadcast |

Rules: shortcuts are handled by one `juce::ApplicationCommandManager` + `KeyPressMappingSet` in `OperatorRoot`, so the map is data (and shown in Help). Keys never trigger Start/Stop recording, Start/Stop broadcast or Arm (explicit buttons only, avoiding ambiguous toggles, spec 11). In CLAP mode the host may take some keys; the strip and the palette button remain.

## 7. Focus rules

1. Initial focus on first launch: the wizard's primary button. Afterwards: the last focused control, restored per tab.
2. Tab order per page is left to right, top to bottom in DOM-like order; the preview is skipped; the bottom strip is the last group, and Ctrl+F6 jumps between groups.
3. A text input captures typing; while it has focus 1 to 8 and T are inert. F-keys and Ctrl-combos still work. Esc leaves the field first, then closes the surrounding panel.
4. Opening a panel moves focus into it; closing returns focus to the opener.
5. Focus ring is always drawn (3:1 against both neighbouring colours); mouse clicks do not hide it on keyboard-initiated focus.
6. Inline confirmation panels (Arm, Start broadcast) default focus to Cancel, not to the dangerous button.
7. JUCE: `setWantsKeyboardFocus`, `setFocusContainerType (FocusContainerType::focusContainer)` for each region, `setExplicitFocusOrder` where layout order differs from tab order; custom `createFocusTraverser()` only if the default breaks the Ctrl+F6 groups (JUCE `Component` API, header read).

## 8. Accessibility

- Every custom component (pad, meter, chip, level slider, preview) overrides `createAccessibilityHandler()` returning a `juce::AccessibilityHandler` with the right `AccessibilityRole`, `setTitle`/`setDescription`/`setHelpText`, and value interfaces for sliders/meters.
- Status changes are announced with `AccessibilityHandler::postAnnouncement (text, AnnouncementPriority)` (present in JUCE 8.0.16): blackout latched/restored, device lost/found, lighting armed/fault, recording started/failed, broadcast live/error. Announcement text uses the same strings as the visible banner.
- State is never colour-only (APP-UI-03): every chip has text; pads have a state word in the accessible name; latched buttons expose a toggle state.
- Platform reality: JUCE ships accessibility backends for Windows (UI Automation), macOS and mobile; the source tree has no Linux backend, so **Linux has no screen reader support** from this layer (honest limit). Not tested with real assistive technology yet.
- Reduced motion: when the OS asks, animations (glow pulse, queued blink) are static. JUCE has no portable query for this, so a manual "Reduce motion" setting exists.
- Strobe safety is also an accessibility matter: Disable Strobing is a first-class toggle in the Setup tab and in the palette. Scenes flagged strobe carry a visible "FLASH" badge; WCAG 2.3.1's three-flashes-per-second threshold is the numeric bound used for Bloom Pulse (NEW-02).
- Test with accessibility off: `juce::AccessibilityHandler` calls must not crash when no client is attached.

## 9. Setup wizard (UJ-01)

Eight steps, full keyboard path, 800x600. Header: "Setup, step N of 8" + progress dots. Footer: [Back] [Skip this step] [Next] and [Cancel]. Enter activates the focused button only; Esc asks "Leave setup? Your changes so far are kept." (Keep, Discard).

| # | Step | Content | Done when |
|---|---|---|---|
| 1 | Welcome | what the wizard does; "Nothing transmits during setup. DMX and broadcast stay off." | Next |
| 2 | Displays | list; [Identify displays] shows big numbers on each display for 5 s; assign Controls and Audience(s); single-display warning; border test pattern button | at least Controls assigned; audience may be Unused (preview only) |
| 3 | Audio | device, route, sample rate, live meter, gain instructions: "Play a loud part of your set. Turn the gain down on your interface until the CLIP light stays off. Aim for peaks around minus 12 to minus 6." (spec 12) | a signal was seen above −40 dBFS, or user skips with a recorded note "audio unverified" |
| 4 | Camera (optional) | list, preview in the wizard window only, "Preview is not published" | skipped or camera chosen |
| 5 | MIDI (optional) | learn 4 core actions: Next scene, Pause, Resume, Visual blackout | skipped or mapped |
| 6 | Lighting interface (optional) | protocol, interface, destination; text "You arm it later on the MIDI/Lighting tab." | skipped or saved (still Disarmed) |
| 7 | Recording folder | picker, free space, write test (creates and deletes a temp file) | writable or skipped |
| 8 | Benchmark and summary | [Run quick benchmark] (cancellable) or [Skip]; summary lists configured, skipped, off; missing media listed (REQ-S08 / UJ-01) | [Finish] saves the profile |

Qualified profile: finishing with a benchmark result stores a profile hash; without it the profile is saved as "unqualified" and Show Doctor says so. Wizard state is persisted after each step so a crash resumes there. Verification: `ui: wizard keyboard path` drives it by `KeyPress` injection only (no mouse) at 800x600 under Xvfb.

## 10. Show Doctor panel (NEW-06)

Location: Diagnostics tab top; also runs automatically on show load and before Arm / Start recording / Start broadcast (shows only Blockers and Warnings inline). Logic is the `doctor` core (pure: takes snapshots, returns findings); the UI renders them.
Finding row: icon+word severity (Blocker, Warning, Info, Pass), title, one-line cause, [Fix] (jumps to the control) or [Details]. Overall banner: "Ready", "Ready with 3 warnings", "Not ready: 1 blocker".

Checks (id, severity rule):
| Id | Check | Blocker if | Warning if |
|---|---|---|---|
| D-AUD-1 | audio device present and callbacks flowing | none open | signal silent for more than 5 s |
| D-AUD-2 | rate/route supported | n/a | unsupported rate |
| D-AUD-3 | input clipping seen in last session minute | n/a | yes |
| D-DSP-1 | audience route exists and matches a display | n/a | route unmatched or only one display |
| D-DSP-2 | no audience route on the controls display | any | n/a |
| D-DSP-3 | identical monitors ambiguous | n/a | yes |
| D-SAF-1 | strobe lock vs scenes/mappings/fixture strobe channels | n/a | locked but flagged items exist (listed) |
| D-MED-1 | media files present/valid, flash-risk media listed | missing media in the active scene | missing elsewhere |
| D-TXT-1 | unpublished text drafts | n/a | yes |
| D-CAM-1 | camera selected but unavailable | n/a | yes; also warn if published unintentionally |
| D-MID-1 | mappings unavailable / feedback alarm | n/a | yes |
| D-LGT-1 | patch conflicts | conflicts and Arm requested | conflicts exist |
| D-LGT-2 | armed state, destination reachability "not verified" (cannot probe without transmitting) | n/a | Armed at show load (should never) |
| D-REC-1 | folder writable, free space | not writable | below 2 GiB (spec 12) |
| D-REC-2 | ffmpeg found/version/encoders (app-adapters 8) | A/V requested, no encoder | n/a |
| D-CST-1 | endpoint configured, secret present | n/a | missing |
| D-PRF-1 | benchmark recent for the current config hash, quality lock | n/a | none/old |
| D-SHW-1 | unsaved changes, autosave age | n/a | more than 5 min unsaved |
| D-SHW-2 | crash-recovery pending | n/a | yes |
| D-THR-1 | thread heartbeats | n/a | any silent over 2 s |
| D-NET-1 | listening sockets at idle | any listening | n/a |

Constraints: running Doctor transmits zero datagrams, opens no recorder/broadcaster, does not start the camera, only enumerates devices (REQ-L03). Runtime budget: under 1 s on a modern desktop excluding device enumeration, which runs on a worker with a 3 s timeout. Test: golden findings for fake adapter states; socket-instrumented null sink asserts zero sends.

## 11. Text Draft / Preview / Publish panel

State machine: Empty -> Draft (dirty) -> Previewing -> Published (live) -> Cleared; any edit while Published returns the editor to Draft but the live text stays until Publish or Clear.
Controls: message list (up to 256), multiline editor with live count "123 / 4096" (Unicode code points, not bytes; counted with `ss::utf8` helpers; paste over the limit is truncated with a note), placement X/Y, size, color picker (Rgb), duration seconds, fade seconds, scroll toggle (bounded), target selector (same Target as Live, defaulting to the Live selection), [Preview] (renders only in the operator preview with a "PREVIEW" watermark), [Publish to: Main] (label names the target; if target is "All visual outputs" the label says so), [Clear published].
Rules: Enter inserts a newline; Ctrl+Enter publishes only when the editor has focus and Preview was shown for the current text; typing never reaches the scene pads (APP-UI-05); publishing while strobe-locked is unaffected (text has no flash); long text beyond the render limit is rejected by validation with a visible message and nothing changes on screen. Audience outputs change only on Publish (REQ-S05). Undo applies to the draft editor text natively, to the message list through the show undo stack.
Test: `ui: text publish isolation` asserts the render mailbox for each audience output is unchanged after typing and Preview, and changes only for targeted outputs after Publish.

## 12. Command Palette (NEW-07)

- Open: Ctrl+K or the palette button in the status bar. Overlay inside the operator window, 560 px wide, anchored 72 px from the top, input plus up to 8 visible rows, scrollable.
- Source: the action registry (`ss::ActionId` list in `core/base/include/ss/base/actions.hpp`) plus `palette` core metadata: display name, category, aliases, default shortcut, "needs argument" schema, enabled-in-context predicate and disabled reason. Opening the palette lists recent first; typing does fuzzy subsequence match on name and aliases (case-insensitive, ranked by prefix, word start, recency).
- Execution: Enter submits an `Action` with `ActionSource::Ui` through the same queue as buttons. Rejections show the sanitised reason inline. Parameterised actions open an inline argument row ("Set BPM: [128]" with range hint); invalid input is not submitted.
- Safety: safety-critical actions are always listed, and rank first for their keywords (blackout, restore, disarm, stop, strobe). `StartRecording`, `StartBroadcast`, `ArmLighting` from the palette open the same inline confirmation as their buttons: the palette cannot arm or transmit in one keystroke. Stop-type actions run immediately.
- Behaviour: Esc closes and returns focus; closing never triggers anything; the palette is non-modal for F8 (F8 still blacks out while it is open).
- Accessibility: input has role combo box, list has role list with selected-item announcements.
- Limits: the palette does not run arbitrary scripts or shell commands (spec non-goal: no arbitrary scripting).
- Test: registry coverage test (every `ActionId` except `None` has palette metadata or an explicit "not user-invocable" flag), fuzzy ranking fixtures, safety ordering, confirmation-required set equals {StartRecording, StartBroadcast, ArmLighting, StopRecording? no}. Exact set is in the `palette` module spec.

## 13. Undo/Redo and A/B snapshots (NEW-03)

- Undoable: edits to the show document (scene parameters via sliders at gesture end, effect settings, cue edits, text messages, MIDI mappings, fixture patches, output settings, media assignments). Slider drags and typing coalesce into one step after 500 ms idle or on release.
- Not undoable: blackout latches, pause/resume, arm/disarm, recording, broadcast, device selection, panic actions, tempo taps (these are performance actions, not edits).
- Depth: at least 100 steps, memory capped at 8 MiB; oldest dropped silently with Diagnostics counter. Undo history is cleared on opening another show; kept across Save.
- Interaction with live show: Undo of a parameter change applies like any other change (through `show`), cue timing continues; Undo of a deleted scene that is currently active is refused with a reason.
- A/B snapshots: Store captures scene id, `SceneParams` (including effect amounts and prism segments), and target output; Recall applies it with a crossfade (default 1.0 s, 0 = cut, range 0 to 10 s). The slot indicator shows "A" when current look equals the stored one, "A*" when changed since recall. Snapshots are saved in the show. Recall while strobe-locked refuses a snapshot that references a strobe scene and says why.
- Undo/Redo and A/B work through `ActionId` values 100 to 105 so MIDI mapping and the palette get them for free.

## 14. Show Report export (NEW-05)

Dialog (inline panel, Diagnostics tab and palette): range (This session, Since last start, Custom from/to), include checkboxes (Setlist of scene/cue changes, Energy/BPM/key timeline, Markers, Device events, Recording/broadcast transitions), format (HTML, CSV, both), folder, [Preview what is included] (shows category counts and a statement "No secrets, no file paths, no audio or video"), [Export]. Success toast with [Open folder]. Errors: no journal data ("Nothing to report yet."), folder not writable, disk full.
Output rules (logic in `diag`): HTML is a single self-contained file (inline CSS and SVG, no external requests, dark Night styling, accessible table headings); CSV files are UTF-8, RFC 4180 quoting, one header row; any cell that starts with `=`, `+`, `-`, `@`, tab or carriage return is prefixed with a single quote to block spreadsheet formula injection from scene names and marker text; times are UTC ISO-8601 with a local-time column; file name `<sanitised show name>_<UTC timestamp>_<session id>.{html,csv}`; deterministic: same journal in, same bytes out.

## 15. Other overlays and dialogs

- Confirm panels (inline, focus on Cancel): Arm lighting, Start broadcast, Quit while recording/live, Delete mapping, Discard unsaved show.
- Toasts: bottom-left above the strip, 4 s, never cover the strip; important ones also post an announcement; none carry paths or secrets.
- Recovery banner after crash (app-shell 8).
- File dialogs: `juce::FileChooser` asynchronous (`launchAsync`), native dialogs (app-adapters 11), never modal loops (`JUCE_MODAL_LOOPS_PERMITTED=0`).

## 16. Audience-side rules (UI view)

Audience windows draw only the composed frame, plus enabled overlays (text, BPM/key, approved branding). Never: operator panels, diagnostics (unless the per-output diagnostics overlay is on, which needs a confirm and shows a red "DIAGNOSTICS ON" corner tag in the operator preview), stream secrets, file paths, cursor (hidden over audience windows), window chrome. Focus never lands in an audience window by accident: `setWantsKeyboardFocus (false)` and `setInterceptsMouseClicks (false, false)` on the audience root; key events that reach it are forwarded to the operator window command manager.

## 17. Tests and headless verification

H tests run in CI under `xvfb-run` using the standalone binary's `--selftest` entry (app-shell 13) with fake adapters. Display-dependent tests need the X root window property `_NET_WORKAREA` or JUCE finds no displays (app-adapters 4.4).

| Test | Verifies |
|---|---|
| `ui: layout 800x600` | region rects of section 2 and the Live tab table, no overlap, all controls inside page area, heights of controls at least 32 px |
| `ui: compact layouts` | sizes 799x600, 800x599, 640x480, 600x400: compact flag, banner below 640x480, fonts at least 14 px |
| `ui: contrast tokens` | script computes WCAG ratios for every token pair used; fails under thresholds in section 3.1 |
| `ui: copy lint` | no U+2014/U+2013, no banned phrase, no "..." overuse in `strings.cpp`; separately lints Help markdown |
| `ui: keyboard map` | every shortcut in section 6 triggers the right `ActionId` (fake dispatcher); pads inert while a `TextEditor` has focus; F8 works from a text field |
| `ui: focus order` | traversal visits each tab's controls in the specified order; Ctrl+F6 groups; focus ring drawn for each |
| `ui: accessibility tree` | every custom component returns a handler with non-empty name and a role; no crash with accessibility disabled |
| `ui: wizard keyboard path` | complete all 8 steps with keys only; nothing transmitted; final state Off |
| `ui: blackout latch ui` | press each blackout, state latched, Restore enabled only then, announcement posted |
| `ui: text publish isolation` | as section 11 |
| `ui: imperfection invariants` | hit rects and layout identical with Clean mode on/off; none on strip controls |
| `ui: golden screenshots` | render each tab to `juce::Image` via `Component::createComponentSnapshot` at 800x600, Night and High contrast; compare with tolerance (software rendering is deterministic per platform but differs across platforms: goldens are per-OS) |
| `ui: doctor read-only` | run with counting null sinks: zero sends, zero opens |
| `ui: palette` | fuzzy fixtures; every action id has metadata; confirmation set; safety ordering |
| `ui: undo` | coalescing, depth cap, refusal rules, A/B recall crossfade request issued |
| `ui: report export` | produce HTML/CSV from a fixture journal, byte-compare golden; formula-injection cell prefixed; no path or secret strings (regex scan) |

`createComponentSnapshot` renders through JUCE's software renderer, which is why these run without a GPU. Real screen reader and real high-DPI display checks are manual (M).

## 18. JUCE classes used

`juce::Component`, `TabbedComponent` (or custom tab bar for the stencil look), `TextButton`, `ToggleButton`, `Slider`, `ComboBox`, `TextEditor`, `TableListBox`, `Viewport`, `FileChooser` (async), `AlertWindow` (avoided; inline panels instead), `ApplicationCommandManager`, `KeyPressMappingSet`, `LookAndFeel_V4` subclass `CdlLookAndFeel`, `Font`, `Typeface::createSystemTypefaceFor` (bundled font from `BinaryData`), `VBlankAttachment` (preview/meters repaint), `Timer` (30 Hz status), `AccessibilityHandler`, `Desktop::DarkModeSettingListener`, `MessageManager::callAsync`.

## 19. Open items

- Replace the proposed tokens with the CDL design system values and the real stencil face; re-run the contrast test.
- Decide default of Disable Strobing for existing shows (new shows: ON).
- Real assistive-technology pass (Windows Narrator, VoiceOver) is not done.
- Compact-layout mock-ups are table-level only; the first implementation PR should add screenshots to this doc.

## Sources

- [S1] WCAG 2.2 Understanding 1.4.3, 1.4.11, 2.3.1: https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html , https://www.w3.org/WAI/WCAG22/Understanding/non-text-contrast.html , https://www.w3.org/WAI/WCAG22/Understanding/three-flashes-or-below-threshold.html (read 2026-10-20). Contrast numbers: own script using the WCAG relative-luminance formula.
- [S2] JUCE `8.0.16-security` headers: `juce_Component.h` (accessibility, focus APIs), `juce_AccessibilityHandler.h` (`postAnnouncement`), `juce_Desktop.h` (dark mode), `juce_VBlankAttachment.h`, `native/accessibility/` directory listing (no Linux backend).
- [S3] Fonts under OFL in the Google Fonts repository: `ofl/bigshouldersstencil/OFL.txt`, `ofl/allertastencil/OFL.txt`, `ofl/sairastencilone/OFL.txt` on raw.githubusercontent.com/google/fonts (fetched 2026-10-20).
- [S4] StageSignal spec v0.1 sections 11, 12, 13, 16, 19; addendum v0.2; `core/base/include/ss/base/actions.hpp`; `core/model/include/ss/model/show_model.hpp`.

## Added controls (28 Oct 2026)
- **Setup > Audio:** "If audio is lost: keep the show running / show the idle screen" (`setup.audio.losspolicy`, pref `audio.losspolicy`, applied at once) and "True-peak limiter for recording" (`setup.audio.truepeak`, pref `dynamics.truepeak`, applies at the next start; the limiter itself is the opt-in described in `dynamics.md`).
- **Media/Text:** "Show for" (until cleared, 5, 10, 30, 60 s), fade (none, 0.5, 1, 2 s) and "Scroll across" (`text.duration`, `text.fade`, `text.scroll`), used by the next Publish. `text.y` and `text.size` have no control yet (default bottom banner).
- All go through `SetPref`, which now refuses a value outside each key's vocabulary (`AppShell.cpp`) instead of storing it. Tests: `audience: SetPref validates ...`, `audience: operator controls write the saved preferences`.
- Not done: a Setup-tab indicator of the BPM/key overlay state per output (the toggle still always draws "off"), and persisting that toggle in the show file.
