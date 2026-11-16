#include "UI/PerformanceStrip.h"

namespace ss::app {

PerformanceStrip::PerformanceStrip(UiModel& model) : PanelBase(model, "Performance strip") {}
PerformanceStrip::~PerformanceStrip() = default;
void PerformanceStrip::resized() {}

}  // namespace ss::app
