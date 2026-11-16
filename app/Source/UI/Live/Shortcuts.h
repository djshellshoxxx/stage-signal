// Keyboard shortcuts through the palette keymap (core/palette KeyMap::defaults). Owner: ui-live.
// Each focused panel calls handleShortcut() from keyPressed(); keys that are not bound return false so the
// parent can try. Keys never start or stop recording, broadcast or arming (app-ui 6).
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModel.h"

namespace ss::app::live {

// Returns true when the key was bound and an action was submitted (or refused by the queue).
bool handleShortcut(UiModel& model, const juce::KeyPress& key);

}  // namespace ss::app::live
