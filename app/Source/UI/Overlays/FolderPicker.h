// Asynchronous folder choice (app-ui 15: file dialogs are never modal loops). Owner: ui-other.
#pragma once
#include <functional>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>

namespace ss::app {

class FolderPicker {
public:
    // Runs on the message thread. The callback gets the chosen folder only; cancel calls nothing.
    void choose(std::function<void(const juce::File&)> onPicked) {
        chooser_ = std::make_unique<juce::FileChooser>("Choose a folder");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this, onPicked](const juce::FileChooser& c) {
                                  const auto f = c.getResult();
                                  if (f != juce::File()) onPicked(f);
                              });
    }

private:
    std::unique_ptr<juce::FileChooser> chooser_;
};

}  // namespace ss::app
