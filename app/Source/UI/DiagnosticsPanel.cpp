#include "UI/DiagnosticsPanel.h"

namespace ss::app {

DiagnosticsPanel::DiagnosticsPanel(UiModel& model) : PanelBase(model, "Diagnostics") {}
DiagnosticsPanel::~DiagnosticsPanel() = default;
void DiagnosticsPanel::resized() {}

}  // namespace ss::app
