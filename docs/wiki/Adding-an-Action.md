# Adding an Action

An action is a numbered command that any control can send: a button, a MIDI mapping, a palette entry or a remote source. The registry is `ss::ActionId` in `core/base/include/ss/base/actions.hpp`.

## The rule that matters most

**Numeric values are stable.** They are stored in show files (MIDI mappings, key maps). Append new ids at the end of their group. Never renumber, and never reuse a retired number.

## Steps

1. **Pick the number.** Look at the group comments in `actions.hpp`. Use the next free value in the group, or start a new group at a free range. The current ranges include 1 to 11 (scene flow), 20 to 23 (blackout), 30 to 34 (look), 40 to 44 (tempo and key), 50 to 52 (text), 60 to 61 (strobe), 70 to 72 (lighting), 80 to 84 (record and broadcast), 90 to 91 (effects), 100 to 108 (usability), 110 to 112 (output), 120 to 122 (show file).
2. **Safety.** Decide whether the action is safety-critical. If it is, add it to `isSafetyCritical` in the same file. Safety-critical actions skip the queue, bypass rate limits and are never dropped. Only do this for real safety actions. Today's list covers the three blackouts, Restore, DisarmLighting, StopRecording, StopBroadcast and DisableStrobing.
3. **Owner.** Decide which module handles it.
   - Show-state actions go in `ShowEngine::submit` (`core/show/src/engine.cpp`). Undoable edits also need `submitEdit` and the undo rule.
   - Recording, broadcast and lighting actions belong to their own modules. The show engine rejects them with "action not handled by show engine". The app dispatcher must route them. Note that `record`, `broadcast`, `midi`, `router` and `palette` are not in the repo yet.
4. **Validation.** Make sure the engine rejects bad `value` and `target` fields with a clear reason. Stale revisions and unknown outputs are rejected, not guessed.
5. **Persistence.** Show files store action ids for MIDI mappings. The persist layer rejects a mapping to an id that is not in the registry. Adding an id is therefore a schema-visible change if a show file can reference it. Check the MIDI action-id validation in `core/persist/src/show_json.cpp` and extend it if needed.
6. **Palette.** When the palette module exists, the action gets a label, keywords, and a context rule that says when it is valid. Until then, nothing lists it.
7. **Tests.** Add `REQ-*` or `NEW-*` cases to `tests/unit/show/` (`test_safety.cpp` for safety actions, `test_edit.cpp` for undoable edits, `test_programmed.cpp` for cue actions). Test the rejection path as well as the happy path.
8. **Docs.** Update the module spec's Integration API, and the Keyboard map in `docs/specs/app-ui.md` if the action has a key.

## Example: a new action for a scene hold

Suppose you want a "hold current scene" action in the 1 to 11 scene-flow group.

```cpp
// core/base/include/ss/base/actions.hpp (ids 12 to 19 are unused today)
enum class ActionId : std::uint32_t {
    // ...
    NextCue = 9, PrevCue = 10, GoToCue = 11,
    HoldScene = 12,   // appended to the scene-flow group
    // ...
    VisualBlackout = 20, /* ... */
};
```

Then handle `HoldScene` in `ShowEngine::submit`, add a test in `tests/unit/show/test_safety.cpp` or `test_auto.cpp`, and document it in the show spec. It is not safety-critical, so leave `isSafetyCritical` alone.

## Quick check

```text
- [ ] Id appended, no renumbering
- [ ] Safety list decided (and changed only with a reason)
- [ ] Owner module named, other modules reject it
- [ ] Persist knows the id if a file can store it
- [ ] Tests cover accepted and rejected inputs
```
