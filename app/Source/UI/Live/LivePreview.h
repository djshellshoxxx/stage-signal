// Low-rate preview of one output (app-ui 5.1, APP-UI-19): labelled "Preview", never transmits, shows BLACKOUT
// when latched. Repaints only when the output publishes a new frame, capped near 15 FPS. Owner: ui-live.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModel.h"
#include "UI/Audience/FrameImage.h"
#include "UI/Theme.h"

namespace ss::app::live {

class LivePreview final : public juce::Component {
public:
    explicit LivePreview(UiModel& m) : model_(m) { setTitle("Preview"); }

    void setOutput(OutputId id) { output_ = id; last_.reset(); }
    void setBlackout(bool b) { if (b != blackout_) { blackout_ = b; repaint(); } }

    // Called from the owning panel's snapshot callback. Cheap: compares the frame pointer, repaints on change.
    void refresh(juce::uint32 nowMs) {
        auto f = model_.latestFrame(output_);
        if (f == last_ || nowMs - lastPaintMs_ < 66) return;   // at most ~15 FPS
        last_ = f;
        lastPaintMs_ = nowMs;
        if (f) audience::copyFrame(f->pixels, image_);
        repaint();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(theme::bgSunken());
        if (!image_.isNull()) g.drawImageWithin(image_, 0, 0, getWidth(), getHeight(), juce::RectanglePlacement::centred);
        g.setColour(theme::textPrimary());
        g.setFont(juce::FontOptions(theme::kBodyFont));
        g.drawText("Preview", getLocalBounds().reduced(6, 4), juce::Justification::topLeft, false);
        if (blackout_) {
            g.setColour(theme::dangerFill());
            g.fillRect(getLocalBounds().removeFromBottom(26));
            g.setColour(theme::focusRing());
            g.drawText("BLACKOUT", getLocalBounds().removeFromBottom(26), juce::Justification::centred, false);
        }
        if (last_ == nullptr) {
            g.setColour(theme::textSecondary());
            g.drawText("No frame yet", getLocalBounds(), juce::Justification::centred, false);
        }
    }

private:
    UiModel& model_;
    OutputId output_ = ss::kPrimaryOutput;
    std::shared_ptr<const SharedFrame> last_;
    juce::Image image_;
    bool blackout_ = false;
    juce::uint32 lastPaintMs_ = 0;
};

}  // namespace ss::app::live
