// One status chip: read-only, focusable, Enter jumps to the owning tab. Owner: ui-live.
#pragma once
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/Status/StatusFormat.h"
#include "UI/Theme.h"

namespace ss::app::status {

class StatusChip final : public juce::Component {
public:
    StatusChip() { setWantsKeyboardFocus(true); }

    void setChip(const Chip& c, bool compact) {
        if (c.label == chip_.label && c.value == chip_.value && c.level == chip_.level && compact == compact_) return;
        chip_ = c;
        compact_ = compact;
        setTitle(juce::String(c.name()));
        repaint();
    }
    const Chip& chip() const { return chip_; }

    std::function<void()> onActivate;

    void paint(juce::Graphics& g) override {
        const auto color = colourFor(chip_.level);
        g.setColour(theme::bgPanel());
        g.fillRect(getLocalBounds());
        g.setColour(hasKeyboardFocus(true) ? theme::focusRing() : theme::lineControl());
        g.drawRect(getLocalBounds().reduced(1), hasKeyboardFocus(true) ? 2 : 1);
        auto area = getLocalBounds().reduced(5, 2);
        g.setColour(color);
        g.setFont(juce::FontOptions(theme::kBodyFont));
        g.drawText(juce::String::charToString(chip_.glyph()), area.removeFromLeft(12), juce::Justification::centred, false);
        g.setColour(theme::textSecondary());
        if (!compact_) g.drawText(juce::String(chip_.label), area.removeFromTop(area.getHeight() / 2), juce::Justification::topLeft, true);
        g.setColour(color);
        g.drawText(juce::String(chip_.value), area, juce::Justification::bottomLeft, true);
    }

    void mouseUp(const juce::MouseEvent& e) override {
        if (getLocalBounds().contains(e.getPosition()) && onActivate) onActivate();
    }

    bool keyPressed(const juce::KeyPress& k) override {
        if (k == juce::KeyPress::returnKey && onActivate) { onActivate(); return true; }
        return false;
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<juce::AccessibilityHandler>(
            *this, juce::AccessibilityRole::button,
            juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press, [this] { if (onActivate) onActivate(); }));
    }

private:
    static juce::Colour colourFor(Level l) {
        switch (l) {
            case Level::Ok: return theme::stateOk();
            case Level::Info: return theme::accentRain();
            case Level::Warn: return theme::stateWarn();
            case Level::Danger: return theme::stateDanger();
            case Level::Off: return theme::textSecondary();
        }
        return {};
    }
    Chip chip_;
    bool compact_ = false;
};

}  // namespace ss::app::status
