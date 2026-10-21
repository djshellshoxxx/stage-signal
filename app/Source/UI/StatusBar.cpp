#include "UI/StatusBar.h"

namespace ss::app {

StatusBar::StatusBar(UiModel& model) : PanelBase(model, "Status bar") {}
StatusBar::~StatusBar() = default;
void StatusBar::resized() {}

}  // namespace ss::app
