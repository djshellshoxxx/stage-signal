#include "UI/OperatorRoot.h"

namespace ss::app {

OperatorRoot::OperatorRoot(UiModel& model) : model_(model), status_(model), tabs_(model), strip_(model), poller_(model) {
    addAndMakeVisible(status_);
    addAndMakeVisible(tabs_);
    addAndMakeVisible(strip_);
    setSize(theme::kDefaultW, theme::kDefaultH);
}

OperatorRoot::~OperatorRoot() = default;

void OperatorRoot::paint(juce::Graphics& g) { g.fillAll(theme::bgNight()); }

void OperatorRoot::resized() {
    auto r = getLocalBounds();
    compact_ = theme::isCompact(r.getWidth(), r.getHeight());
    status_.setBounds(r.removeFromTop(theme::kStatusBarH));
    strip_.setBounds(r.removeFromBottom(theme::kStripH));
    tabs_.setBounds(r);          // tab bar (36) + page area (456 at 800x600)
}

}  // namespace ss::app
