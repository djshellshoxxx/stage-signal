#include "UI/SetupPanel.h"

namespace ss::app {

SetupPanel::SetupPanel(UiModel& model) : PanelBase(model, "Setup") {}
SetupPanel::~SetupPanel() = default;
void SetupPanel::resized() {}

}  // namespace ss::app
