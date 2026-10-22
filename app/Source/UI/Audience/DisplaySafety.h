// Conservative audience-window placement checks. JUCE has no stable display identity: a topology
// change invalidates the operator's explicit selection and needs another confirmation (REQ-D05/D06).
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ss::app::audience {

inline bool sameDisplayPlacement(const juce::Displays::Display& a, const juce::Displays::Display& b) {
    return a.logicalBounds == b.logicalBounds && a.physicalBounds == b.physicalBounds
        && juce::exactlyEqual(a.scale, b.scale) && a.isMain == b.isMain;
}

inline bool sameDisplayLayout(const juce::Array<juce::Displays::Display>& a,
                              const juce::Array<juce::Displays::Display>& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (!sameDisplayPlacement(a.getReference(i), b.getReference(i))) return false;
    return true;
}

inline bool audiencePlacementAllowed(const juce::Displays::Display& selected,
                                     const juce::Array<juce::Displays::Display>& current,
                                     juce::Rectangle<int> operatorBounds, juce::String& reason) {
    if (operatorBounds.isEmpty()) {
        reason = "The operator window position is unknown. Audience output stays closed.";
        return false;
    }
    int matches = 0;
    for (const auto& display : current) if (sameDisplayPlacement(display, selected)) ++matches;
    if (matches != 1 || selected.logicalBounds.isEmpty()) {
        reason = "That display changed or disconnected. Refresh displays and confirm an audience display.";
        return false;
    }
    if (selected.logicalBounds.toNearestInt().intersects(operatorBounds)) {
        reason = "That display holds the operator window. Pick another display for audience output.";
        return false;
    }
    return true;
}

inline std::string displaySelectionToken(const juce::Displays::Display& display) {
    // This is a conservative local geometry hint, never claimed to be an EDID or portable identity.
    return display.logicalBounds.toNearestInt().toString().toStdString() + "/"
        + display.physicalBounds.toString().toStdString() + "/" + std::to_string(display.scale);
}

inline bool audienceWindowContained(const juce::Displays::Display& selected, juce::Rectangle<int> audienceBounds,
                                    juce::Rectangle<int> operatorBounds) {
    return !audienceBounds.isEmpty() && selected.logicalBounds.toNearestInt().contains(audienceBounds)
        && !audienceBounds.intersects(operatorBounds);
}

}  // namespace ss::app::audience
