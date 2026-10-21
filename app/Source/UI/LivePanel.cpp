#include "UI/LivePanel.h"

namespace ss::app {

LivePanel::LivePanel(UiModel& model) : PanelBase(model, "Live") {}
LivePanel::~LivePanel() = default;
void LivePanel::resized() {}

}  // namespace ss::app
