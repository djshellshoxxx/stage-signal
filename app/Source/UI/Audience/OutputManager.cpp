#include "UI/Audience/OutputManager.h"
#include "UI/Audience/DisplaySafety.h"

namespace ss::app::audience {

OutputManager::OutputManager(UiModel& model) : model_(model) {
    model_.addListener(this);
    startTimerHz(20);  // Hotplug safety must not depend on CONTROL producing another snapshot.
}

OutputManager::~OutputManager() {
    stopTimer();
    model_.removeListener(this);
    closeAll();
}

bool OutputManager::open(OutputId id, const juce::Displays::Display& display, juce::Component* operatorComponent,
                         juce::String& reason) {
    if (isOpen(id)) { reason = "This output is already open."; return false; }
    const auto& current = juce::Desktop::getInstance().getDisplays().displays;
    const auto operatorBounds = operatorComponent ? operatorComponent->getScreenBounds() : juce::Rectangle<int>();
    if (!audiencePlacementAllowed(display, current, operatorBounds, reason)) return false;
    auto w = std::make_unique<AudienceWindow>(model_, id, juce::String("Audience " + std::to_string(id)));
    w->showOn(display.logicalBounds.toNearestInt());
    windows_.push_back({id, std::move(w), display, current,
                        juce::Component::SafePointer<juce::Component>(operatorComponent)});
    safetyNotice_.clear();
    return true;
}

void OutputManager::close(OutputId id) {
    for (auto it = windows_.begin(); it != windows_.end(); ++it) {
        if (it->id != id) continue;
        it->window->setVisible(false);
        windows_.erase(it);
        return;
    }
}

void OutputManager::closeAll() {
    for (auto& e : windows_) e.window->setVisible(false);
    windows_.clear();
}

bool OutputManager::isOpen(OutputId id) const {
    for (const auto& e : windows_)
        if (e.id == id) return true;
    return false;
}

void OutputManager::uiSnapshotChanged(const UiSnapshot&) {
    validateWindows();
    for (auto& entry : windows_) entry.window->refresh();
}

void OutputManager::timerCallback() { validateWindows(); }

void OutputManager::validateWindows() {
    const auto& current = juce::Desktop::getInstance().getDisplays().displays;
    for (auto it = windows_.begin(); it != windows_.end();) {
        juce::String reason;
        const auto operatorBounds = it->operatorComponent ? it->operatorComponent->getScreenBounds() : juce::Rectangle<int>();
        const bool layoutUnchanged = sameDisplayLayout(it->topology, current);
        if (!layoutUnchanged || !audiencePlacementAllowed(it->display, current, operatorBounds, reason)
            || !audienceWindowContained(it->display, it->window->getScreenBounds(), operatorBounds)) {
            // Never let the OS migrate a disconnected audience window onto controls. Reopening always needs a click.
            it->window->setVisible(false);
            it = windows_.erase(it);
            safetyNotice_ = "Audience output closed because a display or the operator window moved. Confirm its display to reopen.";
            continue;
        }
        ++it;
    }
}

}  // namespace ss::app::audience
