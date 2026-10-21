// Circuit Drift Labs night theme tokens (docs/specs/app-ui.md section 3.1) and region geometry (section 2).
// Owner: shell-integration for the token values; ui-* workers may ADD helpers in their own files but must not
// edit values here (the contrast test reads this header). Proposed tokens until the CDL design system lands.
#pragma once
#include <juce_graphics/juce_graphics.h>

namespace ss::app::theme {

// ---- colour tokens ---------------------------------------------------------------------------------------
inline juce::Colour bgNight()      { return juce::Colour(0xff04060b); }
inline juce::Colour bgPanel()      { return juce::Colour(0xff0b0f18); }
inline juce::Colour bgRaised()     { return juce::Colour(0xff131a27); }
inline juce::Colour bgSunken()     { return juce::Colour(0xff020306); }
inline juce::Colour textPrimary()  { return juce::Colour(0xffe9eef7); }
inline juce::Colour textSecondary(){ return juce::Colour(0xffa3aec4); }
inline juce::Colour textMuted()    { return juce::Colour(0xff7f8ba3); }
inline juce::Colour accentSpray()  { return juce::Colour(0xffff4fa8); }   // "you chose this"
inline juce::Colour accentRain()   { return juce::Colour(0xff3fe0ff); }   // "the machine reports this"
inline juce::Colour stateOk()      { return juce::Colour(0xff4ade80); }
inline juce::Colour stateWarn()    { return juce::Colour(0xfffbbf24); }
inline juce::Colour stateDanger()  { return juce::Colour(0xffff5c6c); }
inline juce::Colour dangerFill()   { return juce::Colour(0xffb3122b); }
inline juce::Colour lineControl()  { return juce::Colour(0xff6c7a9a); }
inline juce::Colour lineDivider()  { return juce::Colour(0xff44506b); }
inline juce::Colour focusRing()    { return juce::Colour(0xffffffff); }
inline juce::Colour textOnAccent() { return juce::Colour(0xff04060b); }

// ---- geometry (logical px, 800x600 client) ---------------------------------------------------------------
constexpr int kStatusBarH = 48;
constexpr int kTabBarH = 36;
constexpr int kStripH = 60;
constexpr int kDefaultW = 800, kDefaultH = 600;
constexpr int kMinW = 640, kMinH = 480;
constexpr int kCompactW = 800, kCompactH = 600;      // below either: compact layout (section 2)
constexpr float kBodyFont = 14.0f;                   // never smaller (spec 11)

inline bool isCompact(int w, int h) { return w < kCompactW || h < kCompactH; }
inline bool isTooSmall(int w, int h) { return w < kMinW || h < kMinH; }

}  // namespace ss::app::theme
