# palette: action registry, keyboard map, Command Palette (`ss::palette`)

Owner: worker `feat/midi` (same PR as midi). Depends on `base` only (no `show` edge; the one value the registry needs from show,
the key index convention, is defined below). Spec refs: section 11 (keyboard defaults, focus rules), REQ-X02, REQ-D11,
T-UI logic. DEC-13 Command Palette (Ctrl+K).

## Scope
1. **Registry** (`registry.hpp`): metadata for EVERY `ss::ActionId`: display name, category, description, value spec
   (kind, unit, range, integer flag, default, UI offset), default shortcut text, keywords, safety-critical flag (mirrors
   `ss::isSafetyCritical`). `parseValue` / `makeAction` turn operator text into an IF-02 `ss::Action`.
2. **Keyboard map** (`keymap.hpp`): `KeyChord`, defaults (F1 help, F5 next scene, F6 pause, F7 resume, F8 emergency blackout
   latch, Shift+F8 restore, Ctrl+S save, Ctrl+K palette, Ctrl+Z / Ctrl+Y undo/redo, 1-8 scene pads = SceneSelect slot 0..7),
   `shouldDispatch(chord, textFieldFocused)`, `KeyMap::dispatch`, conflict detection, user remapping, override export/import.
3. **Command Palette** (`command_palette.hpp`): deterministic fuzzy search with ranges for highlighting.
4. **Help text** (`help.hpp`): keyboard sheet for the Help tab and the F1 overlay.

## Rules
- Completeness: `test_registry` parses `core/base/include/ss/base/actions.hpp` (path baked in by the test CMake) and fails when
  any enumerator lacks metadata, has mismatching metadata, or the registry has a phantom entry. Names must be unique and
  default-shortcut text must agree with `KeyMap::defaults()`.
- Typing rule (`shouldDispatch`): with a text field focused, a chord is withheld when it would type, edit or move the caret:
  printable keys with at most Shift, Space/Enter/Tab/Backspace/Delete/Insert/arrows/Home/End/Page keys, the chords a text control
  owns (Ctrl+A/C/V/X/Z/Y, so Ctrl+Z is text undo while typing), and AltGr (Ctrl+Alt+printable). Function keys and other chords
  (Ctrl+S, Ctrl+K, F1) still dispatch. So scene pads never fire while typing and F8 emergency blackout is always reachable.
  The app must pass `textFieldFocused = true` during IME composition and for the palette's own search box.
- Remap rules: a chord holds one binding; binding a taken chord fails and names the owner unless `replaceConflict`; a
  safety-critical action can only be bound to chords that still dispatch while typing (so it cannot be put on a letter or
  Space); unknown actions, bad values and "no key" are rejected; `rebind` is all-or-nothing. Imported sets resolve duplicate chords
  first-wins and report the rejects (`fromBindings`).
- Overrides: `exportOverrides()` lists only differences from defaults as `<chord>=<actionId>:<value>`, `<chord>=help`,
  `<chord>=-` (default removed). `importOverrides` is all-or-nothing and applied over the defaults.
- Search: ASCII case-insensitive subsequence match of the query (spaces ignored) against the name with the best alignment:
  prefix +40, word/camelCase boundary +25, consecutive +20, gap -1 per skipped character, leading gap -1 up to 10, exact name +100,
  name starts with query +30. Multi-word queries also match as "every word somewhere" (name, keywords at 60%, category 40%,
  description 25%) in any order. Recently used (MRU, max 12): +40 minus 3 per rank (min 4). An action with a shortcut gets +6 so
  primary live actions win ties ("next" -> Next scene before Next cue). Order: score desc, name, id. Category filter and limit
  (default 50). Empty query: recents first, then category order and name. `ranges` are byte ranges in the display name.
- Value entry: Normalized accepts `0.5` or `50%`; scene pads are typed 1..8 and stored 0..7; BPM 20..300; modes by name or 0..2;
  out of range is an error, never clamped.

## Registry conventions needing owner agreement
`SetKey` value 0..11 = major tonic C..B, 12..23 = minor C..B. `SetBpm` carries BPM (the MIDI module scales a CC to 20..300).
`GoToCue` 0..9999, `TextSelect` message id, `SelectOutput` output id (>= 1).

## Status
- Done and tested: registry, keymap, dispatch rule, conflict detection and remapping, override persistence format, palette
  search/ranking/highlight ranges, help text. 40 doctest cases, ASan/UBSan clean.
- REQ-D11: palette and keymap side done (the five look controls and next-scene are reachable from the keyboard by name with a typed
  value, F5 for next scene). There is no dedicated key for brightness/speed/intensity by default; users can bind them
  (`Binding::value` is carried in the Action). Applying the action to the active scene belongs to `show`.
- Not done here (app layer): translating JUCE key events to `KeyChord`, focus tracking, rendering the palette/help, storing the
  overrides and recents in settings (persist), non-ASCII fuzzy matching (bytes >= 0x80 only match exactly).
- REQ-X02 "visible focus and non-colour state labels": the logic supplies text labels (`[SAFETY]`, "takes a value"); visual focus
  rings are UI work and not verified here.

## Integration API
```cpp
#include "ss/palette/command_palette.hpp"   // namespace ss::palette; links ss::palette (base)
#include "ss/palette/help.hpp"

KeyMap keys = KeyMap::defaults();           // then keys.importOverrides(settingsLines) (ignore failure -> defaults)
// JUCE keyPressed -> chord. Letters/digits as upper-case Key::A.., Key::D0..; modifiers from ModifierKeys.
KeyChord chord{Key::F8, kModNone};
DispatchResult r = keys.dispatch(chord, textFieldHasFocus);
if (r.kind == DispatchResult::Kind::Action) show.submit(r.action);      // source == Keyboard, value = binding value
else if (r.kind == DispatchResult::Kind::Ui && r.ui == UiCommand::ShowKeyboardHelp) openHelpSheet();
// r.kind == Ignored: let the focused widget handle the key (r.ignoredBecause: "unbound" | "text field has focus")
bool consume = shouldDispatch(chord, textFieldHasFocus);              // when only the policy is needed

// Remap UI
Status s = keys.rebind(from, to);           // fails on conflict; keys.conflictFor(to)->action names the owner; replace flag to override
keys.bind(Binding{chord, ActionId::Brightness, UiCommand::None, 0.5});   // value-bearing binding
settings.save(keys.exportOverrides());       // vector<string>

// Command Palette (Ctrl+K opens; the palette's search box counts as a text field)
CommandPalette pal(&keys);                   // pal.setRecent(settings.recent) on start
auto results = pal.search(typedText, SearchOptions{categoryOrNullopt, 50});   // per keystroke; cheap (< 60 actions)
// SearchResult: id, score, nameRanges (highlight in Registry::find(id)->name), shortcut, takesValue, safetyCritical, recent
const ActionInfo* info = Registry::find(results[0].id);
auto act = makeAction(results[0].id, valueTextOrEmpty, ActionSource::Keyboard);   // Result<Action>; show error text on failure
if (act.ok()) { show.submit(act.value()); pal.noteUsed(results[0].id); settings.recent = pal.recent(); }

// Help tab and F1 sheet (same text)
std::string text = keyboardHelpText(keys);   // or keyboardHelpRows(keys) for a table; shortcutText(keys, id) for one cell
// Registry for menus/tooltips: Registry::all(), Registry::find(id), categoryName(cat), parseCategory(str, cat)
```
