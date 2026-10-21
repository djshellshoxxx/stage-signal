#include "UI/PerformanceStrip.h"
#include "UI/Live/Actions.h"
#include "UI/Live/Shortcuts.h"

namespace ss::app {

using uikit::sendAction;

namespace {
void paintLatched(juce::TextButton& b, bool latched, juce::Colour idle) {
    b.setColour(juce::TextButton::buttonColourId, latched ? theme::dangerFill() : idle);
    b.setColour(juce::TextButton::buttonOnColourId, latched ? theme::dangerFill() : idle);
    b.setColour(juce::TextButton::textColourOffId, latched ? theme::focusRing() : theme::textPrimary());
    b.setColour(juce::TextButton::textColourOnId, latched ? theme::focusRing() : theme::textPrimary());
}
}  // namespace

PerformanceStrip::PerformanceStrip(UiModel& model) : PanelBase(model, "Performance strip") {
    auto add = [this](juce::Component& c, const char* id) { c.setComponentID(id); addAndMakeVisible(c); };
    add(visual_, "strip.visual");
    add(lighting_, "strip.lighting");
    add(emergency_, "strip.emergency");
    add(restore_, "strip.restore");
    add(pause_, "strip.pause");
    add(resume_, "strip.resume");
    add(next_, "strip.next");
    add(brightness_, "strip.brightness");
    add(intensity_, "strip.intensity");

    visual_.setButtonText("Visual");
    visual_.setTooltip("Visual blackout (Ctrl+F8). Latched until you restore.");
    visual_.setTitle("Visual blackout");
    visual_.onClick = [this] {
        if (lastVisual_) { announce("Visual blackout is already on. Restore to resume."); return; }
        sendAction(model_, ss::ActionId::VisualBlackout);
    };
    lighting_.setButtonText("Lights");
    lighting_.setTooltip("Lighting blackout (Ctrl+Shift+F8). Latched until you restore. Works with lighting off too.");
    lighting_.setTitle("Lighting blackout");
    lighting_.onClick = [this] {
        if (lastLighting_) { announce("Lighting blackout is already on. Restore to resume."); return; }
        sendAction(model_, ss::ActionId::LightingBlackout);
    };
    emergency_.setButtonText("EMERGENCY");
    emergency_.setTooltip("Emergency blackout (F8). Blacks out visual and lighting. Never stops recording.");
    emergency_.setTitle("Emergency blackout");
    emergency_.onClick = [this] { sendAction(model_, ss::ActionId::EmergencyBlackout); };
    restore_.setButtonText("Restore");
    restore_.setTooltip("Restore visual and lighting (Shift+F8)");
    restore_.setTitle("Restore visual and lighting");
    restore_.onClick = [this] { sendAction(model_, ss::ActionId::RestoreBlackout); };
    restore_.setEnabled(false);

    pause_.setButtonText("Pause");
    pause_.setTooltip("Pause automation (F6)");
    pause_.onClick = [this] { sendAction(model_, ss::ActionId::Pause); };
    resume_.setButtonText("Resume");
    resume_.setTooltip("Resume automation (F7)");
    resume_.onClick = [this] { sendAction(model_, ss::ActionId::Resume); };
    next_.setButtonText("Next");
    next_.setTooltip("Next scene (F5). In Programmed mode this goes to the next cue.");
    next_.onClick = [this] { sendAction(model_, ss::ActionId::NextScene); };

    brightness_.setRange(0.0, 100.0, 1.0);
    intensity_.setRange(0.0, 100.0, 1.0);
    for (auto* s : {&brightness_, &intensity_}) {
        s->setSliderStyle(juce::Slider::LinearHorizontal);
        s->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    }
    brightness_.setTitle("Brightness");
    brightness_.setTooltip("Brightness, 0 to 100. Alt+Up and Alt+Down when focused.");
    intensity_.setTitle("Scene intensity");
    intensity_.setTooltip("Scene intensity, 0 to 100");
    // Sent on release, and on each keyboard step; snapshot-driven moves (syncing_) never send.
    brightness_.onDragEnd = [this] { if (!syncing_) sendAction(model_, ss::ActionId::Brightness, brightness_.getValue() / 100.0); };
    intensity_.onDragEnd = [this] { if (!syncing_) sendAction(model_, ss::ActionId::Intensity, intensity_.getValue() / 100.0); };
    brightness_.onValueChange = [this] { if (!syncing_ && !brightness_.isMouseButtonDown()) sendAction(model_, ss::ActionId::Brightness, brightness_.getValue() / 100.0); };
    intensity_.onValueChange = [this] { if (!syncing_ && !intensity_.isMouseButtonDown()) sendAction(model_, ss::ActionId::Intensity, intensity_.getValue() / 100.0); };
}

PerformanceStrip::~PerformanceStrip() = default;

void PerformanceStrip::paint(juce::Graphics& g) {
    g.fillAll(theme::bgPanel());
    g.setColour(theme::lineDivider());
    g.drawHorizontalLine(0, 0.0f, static_cast<float>(getWidth()));
}

void PerformanceStrip::resized() {
    // Fixed widths for the buttons, the two sliders take what is left (app-ui 4.2 order).
    const int pad = 8, h = getHeight() - 2 * 8;
    auto r = getLocalBounds().reduced(pad, 8);
    auto take = [&r, h](juce::Component& c, int w) {
        c.setBounds(r.removeFromLeft(w).withHeight(h));
        r.removeFromLeft(3);
    };
    take(visual_, 96);
    take(lighting_, 96);
    take(emergency_, 130);
    take(restore_, 92);
    r.removeFromLeft(8);
    take(pause_, 60);
    take(resume_, 72);
    take(next_, 60);
    r.removeFromLeft(8);
    const int half = (r.getWidth() - 3) / 2;
    brightness_.setBounds(r.removeFromLeft(half).withHeight(h));
    r.removeFromLeft(3);
    intensity_.setBounds(r.withHeight(h));
}

bool PerformanceStrip::keyPressed(const juce::KeyPress& key) { return live::handleShortcut(model_, key); }

void PerformanceStrip::uiSnapshotChanged(const UiSnapshot& s) {
    const auto& sh = s.show;
    const bool visual = sh.visualBlackout || sh.emergencyBlackout;
    const bool lighting = sh.lightingBlackout || sh.emergencyBlackout;
    const bool emergency = sh.emergencyBlackout;

    visual_.setButtonText(sh.visualBlackout ? "Visual ON" : "Visual");
    visual_.setTitle(sh.visualBlackout ? "Visual blackout ON" : "Visual blackout");
    paintLatched(visual_, sh.visualBlackout, theme::bgRaised());
    lighting_.setButtonText(sh.lightingBlackout ? "Lights ON" : "Lights");
    lighting_.setTitle(sh.lightingBlackout ? "Lighting blackout ON" : "Lighting blackout");
    paintLatched(lighting_, sh.lightingBlackout, theme::bgRaised());
    emergency_.setButtonText(emergency ? "EMERGENCY ON" : "EMERGENCY");
    paintLatched(emergency_, emergency, theme::dangerFill());
    restore_.setEnabled(visual || lighting);
    restore_.setTooltip(visual || lighting ? "Restore visual and lighting (Shift+F8)" : "Nothing is blacked out");
    pause_.setEnabled(!sh.paused);
    resume_.setEnabled(sh.paused);

    if (seenFirst_) {
        if (sh.visualBlackout != lastVisual_) announce(sh.visualBlackout ? "Visual blackout on." : "Visual blackout restored.");
        if (sh.lightingBlackout != lastLighting_) announce(sh.lightingBlackout ? "Lighting blackout on." : "Lighting blackout restored.");
        if (emergency != lastEmergency_ && emergency) announce("Emergency blackout on.");
    }
    lastVisual_ = sh.visualBlackout;
    lastLighting_ = sh.lightingBlackout;
    lastEmergency_ = emergency;
    seenFirst_ = true;

    // Sliders follow the selected output unless the operator is dragging them.
    if (const auto* o = sh.output(sh.selectedOutput == ss::kAllOutputs ? ss::kPrimaryOutput : sh.selectedOutput)) {
        if (!brightness_.isMouseButtonDown()) {
            syncing_ = true;
            brightness_.setValue(o->params.brightness * 100.0, juce::dontSendNotification);
            syncing_ = false;
        }
        if (!intensity_.isMouseButtonDown()) {
            syncing_ = true;
            intensity_.setValue(o->params.intensity * 100.0, juce::dontSendNotification);
            syncing_ = false;
        }
    }
}

void PerformanceStrip::uiActionOutcome(const ActionOutcome& o) {
    if (!o.accepted && !o.reason.empty()) announce(o.reason.c_str());
}

void PerformanceStrip::announce(const char* text) {
    juce::AccessibilityHandler::postAnnouncement(juce::String(text), juce::AccessibilityHandler::AnnouncementPriority::high);
}

}  // namespace ss::app
