// Appearance choices from Setup > Appearance (app-ui 3.2, 3.4). Owner: ui-setup.
// Kept in memory for this session. There is no AppCommand for persisted appearance yet, so the choice is lost on
// restart (see the ui-setup report). Theme.h is not edited: panels read these flags when they paint.
#pragma once

namespace ss::app::setup {

enum class ThemeChoice : unsigned char { Night, HighContrast };

struct Appearance {
    ThemeChoice theme = ThemeChoice::Night;
    bool cleanMode = true;        // the hand-made touches of app-ui 3.4 are off when false
    bool reduceMotion = false;    // manual setting; JUCE has no portable query (app-ui 8)
};

inline Appearance& appearance() {
    static Appearance a;
    return a;
}

}  // namespace ss::app::setup
