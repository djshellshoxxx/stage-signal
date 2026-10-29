// Shared helpers for the operator panels (ui-live and ui-setup). Owner: ui-live. JUCE-free logic lives in
// LiveFormat.h; this header only wraps UiModel::submitAction and component lookup for tests.
#pragma once
#include <algorithm>
#include <initializer_list>
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModel.h"

namespace ss::app::uikit {

// Builds an Action from the UI source and submits it. Never blocks. The outcome arrives later through
// UiModelListener::uiActionOutcome, so callers only read the immediate result for up-front refusals.
inline UiModel::Ticket sendAction(UiModel& m, ss::ActionId id, double value = 0.0, OutputId target = ss::kAllOutputs,
                                  ss::ActionSource src = ss::ActionSource::Ui) {
    ss::Action a;
    a.actionId = ss::id(id);
    a.source = src;
    a.target = target;
    a.value = value;
    return m.submitAction(a);
}

// Text for a refused submit: sanitised reason from core, or empty when queued.
inline juce::String refusalText(const UiModel::Ticket& t) {
    return t.result.queued ? juce::String() : juce::String(t.result.reason.empty() ? "Not sent. Try again." : t.result.reason);
}

// Finds a descendant by componentID (used by the self-tests and by keyboard focus helpers).
inline juce::Component* findById(juce::Component* root, const juce::String& id) {
    if (!root) return nullptr;
    if (root->getComponentID() == id) return root;
    for (auto* c : root->getChildren())
        if (auto* f = findById(c, id)) return f;
    return nullptr;
}

// True when a text entry has keyboard focus. Scene keys and T are inert then (app-ui 7.3).
inline bool textFieldFocused() {
    return dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr;
}

// Places items left to right in a row. Width 0 takes the remaining space; 3 px gaps. Shared by the panels.
inline void placeRow(juce::Rectangle<int> r, std::initializer_list<std::pair<juce::Component*, int>> items) {
    int fixed = 0, flexCount = 0;
    for (const auto& it : items) { if (it.second > 0) fixed += it.second; else ++flexCount; }
    const int gaps = 3 * static_cast<int>(items.size() > 0 ? items.size() - 1 : 0);
    const int flex = flexCount > 0 ? std::max(0, (r.getWidth() - fixed - gaps) / flexCount) : 0;
    int x = r.getX();
    for (const auto& it : items) {
        const int w = it.second > 0 ? it.second : flex;
        if (it.first) it.first->setBounds(x, r.getY(), w, r.getHeight());
        x += w + 3;
    }
}

}  // namespace ss::app::uikit
