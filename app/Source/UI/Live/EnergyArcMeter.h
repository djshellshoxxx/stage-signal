// Energy Arc bar (app-ui 5.1): 0 to 1 level in rain cyan with a phase word and icon shape. Owner: ui-live.
#pragma once
#include <cmath>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/Live/LiveFormat.h"
#include "UI/Theme.h"

namespace ss::app::live {

class EnergyArcMeter final : public juce::Component {
public:
    EnergyArcMeter() { setTitle("Energy arc"); }

    void setValue(float unit, std::uint8_t phase) {
        const float v = unit < 0.f ? 0.f : (unit > 1.f ? 1.f : unit);
        if (std::fabs(v - value_) < 1e-6f && phase == phase_) return;
        value_ = v;
        phase_ = phase;
        setDescription(juce::String(arcPhaseWord(phase)) + ", " + juce::String(percentText(v)));
        repaint();
    }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().reduced(2);
        g.setColour(theme::bgSunken());
        g.fillRect(r);
        g.setColour(theme::lineControl());
        g.drawRect(r, 1);
        auto fill = r.reduced(2);
        g.setColour(theme::accentRain());
        g.fillRect(fill.withWidth(static_cast<int>(fill.getWidth() * value_)));
        // Phase word and a shape cue (steady: bar, build: up arrow, drop: down arrow).
        const juce::String word = arcPhaseWord(phase_);
        const juce::String shape = phase_ == 1 ? "^" : (phase_ == 2 ? "v" : "-");
        g.setColour(theme::textPrimary());
        g.setFont(juce::FontOptions(theme::kBodyFont));
        g.drawText("Energy " + shape + " " + word + "  " + percentText(value_), getLocalBounds().reduced(6, 0),
                   juce::Justification::centredLeft, false);
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::staticText);
    }

private:
    float value_ = 0.f;
    std::uint8_t phase_ = 0;
};

}  // namespace ss::app::live
