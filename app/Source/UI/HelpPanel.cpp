#include "UI/HelpPanel.h"

namespace ss::app {

HelpPanel::HelpPanel(UiModel& model) : PanelBase(model, "Help") {}
HelpPanel::~HelpPanel() = default;
void HelpPanel::resized() {}

}  // namespace ss::app
