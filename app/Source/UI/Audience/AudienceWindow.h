// Full-screen audience window for one output (app-ui 16). No chrome, no cursor, no keyboard focus.
// It paints the composed frame from UiModel::latestFrame(id) and nothing else. Owner: ui-live.
#pragma once
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/UiModel.h"
#include "UI/Audience/FrameImage.h"

namespace ss::app::audience {

class AudienceCanvas final : public juce::Component {
public:
    AudienceCanvas(UiModel& m, OutputId id) : model_(m), output_(id) {
        setWantsKeyboardFocus(false);
        setInterceptsMouseClicks(false, false);
        setMouseCursor(juce::MouseCursor::NoCursor);
    }

    // Message thread. Cheap when nothing changed: compares the frame pointer, copies only new frames.
    void refresh() {
        auto f = model_.latestFrame(output_);
        if (f == frame_) return;
        frame_ = f;
        if (f) copyFrame(f->pixels, image_);
        blackout_ = !f || f->info.blackout;
        repaint();
    }

    void setPlacementGuard(std::function<bool()> guard) { placementAllowed_ = std::move(guard); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::black);
        if (placementAllowed_ && !placementAllowed_()) return;
        if (!blackout_ && !image_.isNull())
            g.drawImageWithin(image_, 0, 0, getWidth(), getHeight(), juce::RectanglePlacement::centred);
    }

private:
    UiModel& model_;
    OutputId output_;
    std::shared_ptr<const SharedFrame> frame_;
    juce::Image image_;
    bool blackout_ = true;
    std::function<bool()> placementAllowed_;
};

class AudienceWindow final : public juce::DocumentWindow {
public:
    AudienceWindow(UiModel& m, OutputId id, const juce::String& name, std::function<bool(juce::Rectangle<int>)> placement)
        : juce::DocumentWindow(name, juce::Colours::black, 0, false), canvas_(m, id), placement_(std::move(placement)) {
        canvas_.setPlacementGuard([this] { return !placement_ || placement_(getScreenBounds()); });
        setUsingNativeTitleBar(false);
        setTitleBarHeight(0);
        setContentNonOwned(&canvas_, false);
        setWantsKeyboardFocus(false);
        setInterceptsMouseClicks(false, false);
    }
    ~AudienceWindow() override { setContentNonOwned(nullptr, false); }

    void refresh() { canvas_.refresh(); }
    void moved() override {
        if (isVisible() && placement_ && !placement_(getScreenBounds())) setVisible(false);
    }
    // The operator closes audience windows from Setup; the window's own close button is not shown.
    void closeButtonPressed() override {}

    // Puts the window on a display and makes it full screen without chrome.
    void showOn(juce::Rectangle<int> displayArea) {
        setBounds(displayArea);
        setVisible(true);
        setFullScreen(true);
    }

private:
    AudienceCanvas canvas_;
    std::function<bool(juce::Rectangle<int>)> placement_;
};

}  // namespace ss::app::audience
