# Command Palette and Shortcuts

**Status: shortcuts planned, palette planned.** The key map below is the design in `docs/specs/app-ui.md` section 6. The palette module (`palette`) is not in the repository. The action registry it will read is in `core/base/include/ss/base/actions.hpp`.

## Command Palette (Ctrl+K)

- Opens from any tab. Esc closes it and returns focus to where you were.
- Lists every user-invocable action. Actions that are not valid right now are shown greyed, with the reason.
- Fuzzy search ranks prefix and word-start matches first. Safety actions rank first for their keywords.
- Running an action goes through the same dispatcher as the buttons. The palette cannot arm lighting, start recording or start broadcast without the same inline confirmation.
- F8 (emergency blackout) still works while the palette is open.
- Screen reader users get the role, name and selected item.

## Key map

| Key | Action | Notes |
|---|---|---|
| F1 | Help tab | |
| F2 / Shift+F2 | Recall A / store A | |
| F3 / Shift+F3 | Recall B / store B | |
| F5 | Next scene (Next cue in Programmed mode) | |
| F6 / F7 | Pause / Resume | |
| F8 | Emergency blackout | Works from any focus, including text fields |
| Shift+F8 | Restore blackouts | |
| Ctrl+F8 | Visual blackout | |
| Ctrl+Shift+F8 | Lighting blackout | |
| 1 to 8 | Scene pad on current bank | Off while a text field has focus |
| T | Tap tempo | Off while a text field has focus |
| Ctrl+S | Save show | |
| Ctrl+Z | Undo | In a text field, acts on the text first |
| Ctrl+Y or Ctrl+Shift+Z | Redo | |
| Ctrl+K | Command Palette | |
| Ctrl+1 to Ctrl+7 | Switch to tab 1 to 7 | |
| Ctrl+Tab / Ctrl+Shift+Tab | Next or previous tab | |
| Ctrl+F6 | Cycle focus: status bar, tabs, page, bottom strip | |
| Tab / Shift+Tab | Next or previous control | |
| Esc | Close palette, cancel Learn or a wizard step, close a popover | Never stops recording or broadcast |

## Rules

- Keys never start or stop recording or broadcast, and never arm lighting. Those need an explicit button press, so there are no ambiguous toggles.
- Shortcuts are in-app only. They are not system-wide hotkeys.
- Keyboard map is data in the app and is shown on the Help tab.
- In CLAP mode a host may swallow some keys (Space, Ctrl+S, Ctrl+K and the F-keys are the usual suspects). Every emergency action also has a mouse route, so blackout still works without the keyboard.

## Testing status

Keyboard and focus behaviour is specified as Xvfb tests in `app-ui.md` section 17. Those tests do not exist yet.
