#include "UI/MidiLightingPanel.h"

namespace ss::app {

MidiLightingPanel::MidiLightingPanel(UiModel& model) : PanelBase(model, "MIDI / Lighting") {}
MidiLightingPanel::~MidiLightingPanel() = default;
void MidiLightingPanel::resized() {}

}  // namespace ss::app
