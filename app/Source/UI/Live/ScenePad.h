// One scene pad (app-ui 5.1): number, name, state word. The state word is drawn and spoken, never colour only.
// Owner: ui-live.
#pragma once
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/Theme.h"

namespace ss::app::live {

class ScenePad final : public juce::Component {
public:
    enum class State { Idle, Active, Queued, Unavailable };

    explicit ScenePad(int slot) : slot_(slot) { setTitle("Scene pad " + juce::String(slot + 1)); }

    void setState(State s, const juce::String& name) {
        if (s == state_ && name == name_) return;
        state_ = s;
        name_ = name;
        setDescription(stateWord());
        repaint();
    }
    State state() const { return state_; }
    juce::String stateWord() const {
        switch (state_) {
            case State::Active: return "Active";
            case State::Queued: return "Queued, plays next";
            case State::Unavailable: return "Unavailable";
            case State::Idle: return "Idle";
        }
        return {};
    }

    std::function<void(int slot)> onPress;

    void paint(juce::Graphics& g) override {
        const auto r = getLocalBounds().toFloat().reduced(0.5f);
        const bool active = state_ == State::Active;
        g.setColour(active ? theme::accentSpray() : theme::bgRaised());
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(state_ == State::Queued ? theme::accentRain() : theme::lineControl());
        g.drawRoundedRectangle(r, 4.0f, state_ == State::Queued ? 2.0f : 1.0f);
        if (hasKeyboardFocus(true)) {
            g.setColour(theme::focusRing());
            g.drawRoundedRectangle(r.expanded(2.0f), 5.0f, 2.0f);
        }
        const auto fg = active ? theme::textOnAccent() : (state_ == State::Unavailable ? theme::textMuted() : theme::textPrimary());
        g.setColour(fg);
        g.setFont(juce::FontOptions(theme::kBodyFont));
        auto area = getLocalBounds().reduced(6, 4);
        g.drawText(juce::String(slot_ + 1), area.removeFromTop(16), juce::Justification::left);
        g.drawText(name_.isEmpty() ? juce::String("Scene " + juce::String(slot_ + 1)) : name_, area.removeFromTop(30),
                   juce::Justification::topLeft, true);
        g.drawText(stateWord(), area, juce::Justification::bottomLeft, false);
    }

    void mouseUp(const juce::MouseEvent& e) override {
        if (isEnabled() && getLocalBounds().contains(e.getPosition()) && onPress) onPress(slot_);
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<juce::AccessibilityHandler>(
            *this, juce::AccessibilityRole::button,
            juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press, [this] {
                if (onPress) onPress(slot_);
            }));
    }

private:
    int slot_;
    State state_ = State::Idle;
    juce::String name_;
};

}  // namespace ss::app::live
