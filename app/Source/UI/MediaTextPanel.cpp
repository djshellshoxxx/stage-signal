#include "UI/MediaTextPanel.h"
#include <initializer_list>
#include "UI/Overlays/OverlayText.h"
#include "UI/Theme.h"
#include "UI/Audience/FrameImage.h"

namespace ss::app {

namespace {
void styleLabel(juce::Label& l, bool primary) {
    l.setFont(overlay::bodyFont(theme::kBodyFont));
    l.setColour(juce::Label::textColourId, primary ? theme::textPrimary() : theme::textSecondary());
}
}  // namespace

MediaTextPanel::MediaTextPanel(UiModel& model) : PanelBase(model, "Media / Text") {
    styleLabel(mediaTitle_, true);
    mediaTitle_.setText("Media", juce::dontSendNotification);
    styleLabel(mediaCount_, false);
    styleLabel(mediaNote_, false);
    mediaNote_.setText("Still images only (PNG, JPEG, BMP; a GIF shows its first frame). Up to three.", juce::dontSendNotification);
    import_.setTooltip("Choose a still image to show over the program.");
    import_.onClick = [this] {
        if (imported_ >= 3) { status_.setText("Three images are loaded. Remove one first.", juce::dontSendNotification); return; }
        chooser_ = std::make_unique<juce::FileChooser>("Choose an image", juce::File(), "*.png;*.jpg;*.jpeg;*.bmp;*.gif");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            AppCommand c;
            c.kind = AppCommandKind::ImportMedia;
            c.text = f.getFullPathName().toStdString();
            c.a = imported_;
            if (overlay::sendCommand(model_, c, &cmdCorrelation_)) status_.setText("Importing.", juce::dontSendNotification);
            else status_.setText("Too many requests waiting. Try again in a moment.", juce::dontSendNotification);
        });
    };
    remove_.setTooltip("Remove the last imported image.");
    remove_.setEnabled(false);
    remove_.onClick = [this] {
        if (imported_ <= 0) return;
        AppCommand c;
        c.kind = AppCommandKind::RemoveMedia;
        c.a = imported_ - 1;
        if (overlay::sendCommand(model_, c, &cmdCorrelation_)) --imported_;
        remove_.setEnabled(imported_ > 0);
    };

    styleLabel(cameraTitle_, true); cameraTitle_.setText("Camera", juce::dontSendNotification);
    styleLabel(cameraStatus_, false);
    cameraDevices_.setTextWhenNothingSelected("Choose a camera");
    auto cameraCommand = [this](AppCommandKind kind) {
        AppCommand c; c.kind = kind; c.a = kPrimaryOutput;
        if (kind == AppCommandKind::OpenCamera) {
            const int index = cameraDevices_.getSelectedId() - 1;
            if (index < 0 || index >= static_cast<int>(cameras_.size())) return;
            c.text = cameras_[static_cast<std::size_t>(index)].id;
        }
        overlay::sendCommand(model_, c, &cmdCorrelation_);
    };
    cameraRefresh_.onClick = [cameraCommand] { cameraCommand(AppCommandKind::RefreshCameras); };
    cameraOpen_.onClick = [cameraCommand] { cameraCommand(AppCommandKind::OpenCamera); };
    cameraClose_.onClick = [cameraCommand] { cameraCommand(AppCommandKind::CloseCamera); };
    cameraPublish_.onClick = [cameraCommand] { cameraCommand(AppCommandKind::PublishCamera); };
    cameraUnpublish_.onClick = [cameraCommand] { cameraCommand(AppCommandKind::UnpublishCamera); };
    cameraOpen_.setComponentID("camera.preview"); cameraPublish_.setComponentID("camera.publish");
    cameraPublish_.setEnabled(false); cameraUnpublish_.setEnabled(false);
    for (auto* c : std::initializer_list<juce::Component*>{&cameraTitle_, &cameraStatus_, &cameraDevices_, &cameraRefresh_,
                                                         &cameraOpen_, &cameraClose_, &cameraPublish_, &cameraUnpublish_, &cameraImage_})
        addAndMakeVisible(c);

    styleLabel(textTitle_, true);
    textTitle_.setText("Text", juce::dontSendNotification);
    styleLabel(count_, false);
    draft_.setMultiLine(true);
    draft_.setReturnKeyStartsNewLine(true);
    draft_.setScrollbarsShown(true);
    draft_.setTextToShowWhenEmpty("Type the message here", theme::textMuted());
    draft_.setFont(overlay::bodyFont(theme::kBodyFont));
    draft_.setColour(juce::TextEditor::backgroundColourId, theme::bgSunken());
    draft_.setColour(juce::TextEditor::textColourId, theme::textPrimary());
    draft_.setColour(juce::TextEditor::outlineColourId, theme::lineControl());
    draft_.setInputRestrictions(kTextLimit);
    draft_.onTextChange = [this] {
        updateCount();
        if (previewing_) setPreview(false);   // an edit clears the preview until it is shown again
    };

    preview_.onClick = [this] { setPreview(!previewing_); };
    styleLabel(previewLabel_, false);
    previewLabel_.setVisible(false);
    publish_.setEnabled(false);
    publish_.setTooltip("Preview the text first. Publish sends exactly what you previewed.");
    publish_.onClick = [this] {
        AppCommand c;
        c.kind = AppCommandKind::PublishText;
        c.text = previewedText_;
        if (overlay::sendCommand(model_, c, &cmdCorrelation_)) status_.setText("Publishing.", juce::dontSendNotification);
        else status_.setText("Too many requests waiting. Try again in a moment.", juce::dontSendNotification);
    };
    styleLabel(publishNote_, false);
    publishNote_.setText("Preview first. Publish shows the previewed text on the program output.", juce::dontSendNotification);
    clear_.onClick = [this] {
        status_.setText(juce::String(overlay::sendAction(model_, ss::ActionId::TextClear, 0.0, kAllOutputs,
                                                         &clearCorrelation_)),
                        juce::dontSendNotification);
    };
    clear_.setTooltip("Clears the published text on the selected outputs.");
    styleLabel(status_, false);
    updateCount();

    for (auto* c : std::initializer_list<juce::Component*>{&mediaTitle_, &mediaCount_, &mediaNote_, &import_, &remove_,
                                                           &textTitle_, &count_, &draft_, &preview_, &previewLabel_,
                                                           &publish_, &clear_, &publishNote_, &status_})
        addAndMakeVisible(c);
}

MediaTextPanel::~MediaTextPanel() = default;

void MediaTextPanel::paint(juce::Graphics& g) {
    g.fillAll(theme::bgPanel());
    g.setColour(theme::lineDivider());
    g.drawVerticalLine(300, 8.0f, static_cast<float>(getHeight() - 8));
}

void MediaTextPanel::updateCount() {
    const auto n = overlay::codePointCount(draft_.getText().toStdString());
    count_.setText(juce::String(static_cast<int>(n)) + " / " + juce::String(kTextLimit), juce::dontSendNotification);
}

void MediaTextPanel::setPreview(bool on) {
    previewing_ = on;
    preview_.setButtonText(on ? "Hide preview" : "Preview");
    publish_.setEnabled(on && draft_.getText().isNotEmpty());
    if (on) {
        previewedText_ = draft_.getText().toStdString();
        previewLabel_.setText("PREVIEW\n" + juce::String(previewedText_), juce::dontSendNotification);
    }
    previewLabel_.setVisible(on);
    resized();
}

void MediaTextPanel::resized() {
    auto left = juce::Rectangle<int>(8, 8, 284, getHeight() - 16);
    mediaTitle_.setBounds(left.removeFromTop(22));
    mediaCount_.setBounds(left.removeFromTop(22));
    mediaNote_.setBounds(left.removeFromTop(36));
    auto lb = left.removeFromTop(30);
    import_.setBounds(lb.removeFromLeft(100).reduced(0, 2));
    lb.removeFromLeft(8);
    remove_.setBounds(lb.removeFromLeft(100).reduced(0, 2));

    left.removeFromTop(8);
    cameraTitle_.setBounds(left.removeFromTop(22));
    cameraStatus_.setBounds(left.removeFromTop(24));
    auto cameraRow = left.removeFromTop(28);
    cameraRefresh_.setBounds(cameraRow.removeFromRight(70)); cameraDevices_.setBounds(cameraRow);
    cameraRow = left.removeFromTop(30);
    cameraOpen_.setBounds(cameraRow.removeFromLeft(140)); cameraClose_.setBounds(cameraRow);
    cameraRow = left.removeFromTop(30);
    cameraPublish_.setBounds(cameraRow.removeFromLeft(140)); cameraUnpublish_.setBounds(cameraRow);
    cameraImage_.setBounds(left.removeFromTop(100));

    auto r = juce::Rectangle<int>(312, 8, getWidth() - 320, getHeight() - 16);
    textTitle_.setBounds(r.removeFromTop(22));
    auto countRow = r.removeFromTop(20);
    count_.setBounds(countRow);
    r.removeFromTop(4);
    draft_.setBounds(r.removeFromTop(150));
    r.removeFromTop(6);
    auto buttons = r.removeFromTop(30);
    preview_.setBounds(buttons.removeFromLeft(110).reduced(0, 2));
    buttons.removeFromLeft(8);
    publish_.setBounds(buttons.removeFromLeft(100).reduced(0, 2));
    buttons.removeFromLeft(8);
    clear_.setBounds(buttons.removeFromLeft(150).reduced(0, 2));
    publishNote_.setBounds(r.removeFromTop(20));
    r.removeFromTop(4);
    if (previewing_) previewLabel_.setBounds(r.removeFromTop(juce::jmin(r.getHeight(), 60)));
    else previewLabel_.setBounds(juce::Rectangle<int>());
    status_.setBounds(r.removeFromTop(22));
}

void MediaTextPanel::uiSnapshotChanged(const UiSnapshot& s) {
    const auto state = static_cast<CameraState>(s.media.cameraState);
    bool changed = cameras_.size() != s.media.cameraDevices.size();
    if (!changed) for (std::size_t i = 0; i < cameras_.size(); ++i) changed = changed || cameras_[i].id != s.media.cameraDevices[i].id;
    if (changed) {
        const auto selected = cameraDevices_.getText();
        cameras_ = s.media.cameraDevices; cameraDevices_.clear(juce::dontSendNotification);
        for (std::size_t i = 0; i < cameras_.size(); ++i) cameraDevices_.addItem(cameras_[i].name, static_cast<int>(i + 1));
        cameraDevices_.setText(selected, juce::dontSendNotification);
    }
    cameraOpen_.setEnabled(state != CameraState::Unsupported && !cameras_.empty());
    cameraPublish_.setEnabled(state == CameraState::Previewing && s.media.cameraPreview != nullptr);
    cameraUnpublish_.setEnabled(state == CameraState::Published || state == CameraState::Lost);
    cameraClose_.setEnabled(state != CameraState::Off && state != CameraState::Unsupported);
    cameraStatus_.setText(state == CameraState::Unsupported ? "Camera unsupported on this platform." :
        state == CameraState::Published ? "Published to Output 1" : state == CameraState::Lost ? "Camera lost; no fallback selected." :
        state == CameraState::Previewing ? "Preview only; not on audience output." : "Camera off", juce::dontSendNotification);
    if (cameraPreview_ != s.media.cameraPreview) {
        cameraPreview_ = s.media.cameraPreview;
        juce::Image image;
        if (cameraPreview_) audience::copyFrame(*cameraPreview_, image);
        cameraImage_.setImage(image);
    }
    mediaCount_.setText(juce::String(static_cast<int>(s.media.loadedItems)) + " items loaded.", juce::dontSendNotification);
    if (!s.media.note.empty()) mediaNote_.setText(juce::String(s.media.note), juce::dontSendNotification);
}

void MediaTextPanel::uiCommandOutcome(const CommandOutcome& o) {
    if (cmdCorrelation_ == 0 || o.correlationId != cmdCorrelation_) return;
    cmdCorrelation_ = 0;
    if (o.kind == AppCommandKind::ImportMedia) {
        if (o.ok) { ++imported_; remove_.setEnabled(true); }
    }
    status_.setText(juce::String(o.message.empty() ? (o.ok ? "Done." : "Not done. Try again.") : o.message), juce::dontSendNotification);
}

void MediaTextPanel::uiActionOutcome(const ActionOutcome& o) {
    if (clearCorrelation_ == 0 || o.correlationId != clearCorrelation_) return;
    clearCorrelation_ = 0;
    if (!o.accepted) status_.setText(juce::String(o.reason.empty() ? "Not cleared. Try again." : o.reason),
                                     juce::dontSendNotification);
}

}  // namespace ss::app
