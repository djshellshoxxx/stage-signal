#include "UI/Audience/OutputManager.h"

namespace ss::app::audience {

OutputManager::OutputManager(UiModel& model) : model_(model) { model_.addListener(this); }

OutputManager::~OutputManager() {
    model_.removeListener(this);
    closeAll();
}

bool OutputManager::open(OutputId id, const juce::Displays::Display& display, const juce::Component* operatorComponent,
                         juce::String& reason) {
    if (isOpen(id)) { reason = "This output is already open."; return false; }
    if (operatorComponent) {
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* here = displays.getDisplayForPoint(operatorComponent->getScreenBounds().getCentre());
        if (here && here->logicalBounds.toNearestInt() == display.logicalBounds.toNearestInt()) {
            reason = "That display holds the operator window. Pick another display for audience output.";
            return false;
        }
    }
    auto w = std::make_unique<AudienceWindow>(model_, id, juce::String("Audience " + std::to_string(id)));
    w->showOn(display.logicalBounds.toNearestInt());
    windows_.push_back({id, std::move(w)});
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
    for (auto& e : windows_) e.window->refresh();
}

}  // namespace ss::app::audience
