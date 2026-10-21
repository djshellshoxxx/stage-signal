#include "UI/LivePanel.h"
#include <algorithm>
#include "UI/Live/Actions.h"
#include "UI/Live/LiveFormat.h"
#include "UI/Live/Shortcuts.h"

namespace ss::app {

using uikit::sendAction;
using live::EnergyArcMeter;
using live::LivePreview;
using live::ScenePad;

// All controls of the Live tab. Positions are the 800x456 table of app-ui 5.1 when wide, a single column when
// compact. The content is scrolled by the parent viewport only in compact layout.
class LivePanel::Content final : public juce::Component, private juce::Slider::Listener {
public:
    explicit Content(UiModel& m) : model_(m), preview_(m) {
        setComponentID("live.content");
        auto add = [this](juce::Component& c, const char* id) { c.setComponentID(id); addAndMakeVisible(c); };
        add(targetBox_, "live.target");
        add(preview_, "live.preview");
        add(tap_, "live.tap");
        add(nudgeDown_, "live.bpm.down");
        add(nudgeUp_, "live.bpm.up");
        add(bpm_, "live.bpm");
        add(releaseBpm_, "live.bpm.release");
        add(keyBox_, "live.key");
        add(tempoInfo_, "live.tempo.info");
        add(arc_, "live.arc");
        add(modeAuto_, "live.mode.auto");
        add(modeProg_, "live.mode.programmed");
        add(modeHybrid_, "live.mode.hybrid");
        add(cueLabel_, "live.cue.label");
        add(cuePrev_, "live.cue.prev");
        add(cueNext_, "live.cue.next");
        for (int i = 0; i < 8; ++i) {
            pads_[i] = std::make_unique<ScenePad>(i);
            add(*pads_[i], ("live.pad." + std::to_string(i + 1)).c_str());
            pads_[i]->onPress = [this](int slot) { send(ss::ActionId::SceneSelect, slot); };
        }
        add(bankPrev_, "live.bank.prev");
        add(bankLabel_, "live.bank.label");
        add(bankNext_, "live.bank.next");
        add(speed_, "live.look.speed");
        add(hue_, "live.look.hue");
        add(sat_, "live.look.sat");
        add(prismOn_, "live.fx.prism.on");
        add(prismAmount_, "live.fx.prism.amount");
        add(bloomOn_, "live.fx.bloom.on");
        add(bloomAmount_, "live.fx.bloom.amount");
        add(undo_, "live.undo");
        add(redo_, "live.redo");
        add(storeA_, "live.a.store");
        add(recallA_, "live.a.recall");
        add(storeB_, "live.b.store");
        add(recallB_, "live.b.recall");
        add(notice_, "live.notice");

        setupText();
        setupButtons();
        setupSliders();
        setupTargetAndKeys();
    }

    juce::Component* control(const juce::String& id) { return uikit::findById(this, id); }
    OutputId target() const { return target_; }
    juce::String notice() const { return notice_.getText(); }
    void setNotice(const juce::String& s) { notice_.setText(s, juce::dontSendNotification); }

    void setCompact(bool c) { compact_ = c; }

    // Lays out for the given width and returns the height the content needs.
    int layout(int width, int minHeight) {
        if (!compact_) return layoutWide(std::max(width, 800), std::max(minHeight, 456));
        return layoutNarrow(width);
    }

    void apply(const UiSnapshot& s) {
        const auto& sh = s.show;
        syncTargets(s);
        syncTempo(s);
        const bool blackout = sh.visualBlackout;
        mode(sh.mode);
        cueLabel_.setText(juce::String(live::cueText(sh.cueIndex, sh.cueCount)), juce::dontSendNotification);
        arc_.setValue(s.features.energyArc, s.features.arcPhase);
        bankLabel_.setText(juce::String(live::bankText(sh.bank, sh.bankCount)), juce::dontSendNotification);
        undo_.setEnabled(sh.undoDepth > 0);
        redo_.setEnabled(sh.redoDepth > 0);
        recallA_.setEnabled(sh.slots[0].used);
        recallB_.setEnabled(sh.slots[1].used);
        if (const auto* o = sh.output(target_ == ss::kAllOutputs ? ss::kPrimaryOutput : target_)) {
            syncEffects(o->params);
        }
        preview_.setOutput(target_ == ss::kAllOutputs ? ss::kPrimaryOutput : target_);
        preview_.setBlackout(blackout);
        preview_.refresh(juce::Time::getMillisecondCounter());
    }

private:
    void setupText() {
        tempoInfo_.setFont(juce::FontOptions(theme::kBodyFont));
        tempoInfo_.setColour(juce::Label::textColourId, theme::accentRain());
        cueLabel_.setFont(juce::FontOptions(theme::kBodyFont));
        cueLabel_.setColour(juce::Label::textColourId, theme::textPrimary());
        bankLabel_.setJustificationType(juce::Justification::centred);
        bankLabel_.setColour(juce::Label::textColourId, theme::textPrimary());
        notice_.setColour(juce::Label::textColourId, theme::stateWarn());
        notice_.setFont(juce::FontOptions(theme::kBodyFont));
        notice_.setMinimumHorizontalScale(1.0f);
    }
    void setupButtons() {
        tap_.setButtonText("Tap");
        tap_.setTooltip("Tap tempo (T). Tap in time with the music.");
        tap_.onClick = [this] { send(ss::ActionId::TapTempo, 0.0); };
        nudgeDown_.setButtonText("-1");
        nudgeDown_.setTooltip("Lower tempo by 1 BPM");
        nudgeDown_.onClick = [this] { nudgeBpm(-1.0); };
        nudgeUp_.setButtonText("+1");
        nudgeUp_.setTooltip("Raise tempo by 1 BPM");
        nudgeUp_.onClick = [this] { nudgeBpm(1.0); };
        releaseBpm_.setButtonText("Release");
        releaseBpm_.setTooltip("Return tempo to the detected estimate. Enabled after a tap or manual change.");
        releaseBpm_.onClick = [this] { send(ss::ActionId::ReleaseBpmOverride, 0.0); };

        modeAuto_.setButtonText("Auto");
        modeProg_.setButtonText("Programmed");
        modeHybrid_.setButtonText("Hybrid");
        for (auto* b : {&modeAuto_, &modeProg_, &modeHybrid_}) {
            b->setClickingTogglesState(true);
            b->setRadioGroupId(1);
        }
        modeAuto_.onClick = [this] { send(ss::ActionId::SetMode, 0.0); };
        modeProg_.onClick = [this] { send(ss::ActionId::SetMode, 1.0); };
        modeHybrid_.onClick = [this] { send(ss::ActionId::SetMode, 2.0); };
        cuePrev_.setButtonText("<");
        cuePrev_.setTooltip("Previous cue");
        cuePrev_.onClick = [this] { send(ss::ActionId::PrevCue, 0.0); };
        cueNext_.setButtonText(">");
        cueNext_.setTooltip("Next cue");
        cueNext_.onClick = [this] { send(ss::ActionId::NextCue, 0.0); };

        bankPrev_.setButtonText("<");
        bankPrev_.setTooltip("Previous bank of scene pads");
        bankPrev_.onClick = [this] { send(ss::ActionId::BankPrev, 0.0); };
        bankNext_.setButtonText(">");
        bankNext_.setTooltip("Next bank of scene pads");
        bankNext_.onClick = [this] { send(ss::ActionId::BankNext, 0.0); };

        prismOn_.setButtonText("Prism Mirror");
        bloomOn_.setButtonText("Bloom Pulse");
        prismOn_.setClickingTogglesState(true);
        bloomOn_.setClickingTogglesState(true);
        prismOn_.onClick = [this] { effectToggled(ss::ActionId::EffectPrismMirror, prismOn_, prismAmount_); };
        bloomOn_.onClick = [this] { effectToggled(ss::ActionId::EffectBloomPulse, bloomOn_, bloomAmount_); };

        undo_.setButtonText("Undo");
        redo_.setButtonText("Redo");
        storeA_.setButtonText("A store");
        recallA_.setButtonText("A recall");
        storeB_.setButtonText("B store");
        recallB_.setButtonText("B recall");
        undo_.onClick = [this] { send(ss::ActionId::Undo, 0.0); };
        redo_.onClick = [this] { send(ss::ActionId::Redo, 0.0); };
        storeA_.onClick = [this] { send(ss::ActionId::SnapshotStoreA, 0.0); };
        recallA_.onClick = [this] { send(ss::ActionId::SnapshotRecallA, 0.0); };
        storeB_.onClick = [this] { send(ss::ActionId::SnapshotStoreB, 0.0); };
        recallB_.onClick = [this] { send(ss::ActionId::SnapshotRecallB, 0.0); };
    }

    void setupSliders() {
        for (auto* s : {&bpm_, &speed_, &hue_, &sat_, &prismAmount_, &bloomAmount_}) {
            s->addListener(this);
            s->setSliderStyle(juce::Slider::LinearHorizontal);
            s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 20);
        }
        bpm_.setRange(40.0, 240.0, 0.5);
        bpm_.setValue(120.0, juce::dontSendNotification);
        bpm_.setTooltip("Tempo in BPM (40 to 240). Release the slider to set it.");
        speed_.setRange(0.0, 1.0, 0.01);
        hue_.setRange(0.0, 1.0, 0.01);
        sat_.setRange(0.0, 1.0, 0.01);
        speed_.setValue(0.5, juce::dontSendNotification);
        hue_.setValue(0.0, juce::dontSendNotification);
        sat_.setValue(0.8, juce::dontSendNotification);
        prismAmount_.setRange(0.0, 1.0, 0.01);
        bloomAmount_.setRange(0.0, 1.0, 0.01);
        prismAmount_.setValue(0.5, juce::dontSendNotification);
        bloomAmount_.setValue(0.5, juce::dontSendNotification);
        speed_.setTitle("Speed");
        hue_.setTitle("Hue");
        sat_.setTitle("Saturation");
        bpm_.setTitle("Tempo BPM");
    }

    void setupTargetAndKeys() {
        targetBox_.onChange = [this] {
            const int idx = targetBox_.getSelectedId();
            for (const auto& e : targetIds_)
                if (e.first == idx) { target_ = e.second; preview_.setOutput(target_ == ss::kAllOutputs ? ss::kPrimaryOutput : target_); }
        };
        keyBox_.addItem("Auto", 1);
        static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        for (int t = 0; t < 12; ++t) {
            keyBox_.addItem(juce::String(kNames[t]) + " major", 2 + t);
            keyBox_.addItem(juce::String(kNames[t]) + " minor", 14 + t);
        }
        keyBox_.setSelectedId(1, juce::dontSendNotification);
        keyBox_.setTooltip("Set the key by hand, or Auto to use the estimate");
        keyBox_.onChange = [this] {
            const int id = keyBox_.getSelectedId();
            if (id == 1) { send(ss::ActionId::ReleaseKeyOverride, 0.0); return; }
            const double v = id >= 14 ? (id - 14) + 12.0 : (id - 2);
            send(ss::ActionId::SetKey, v);
        };
        targetBox_.addItem("All visual outputs", 1);
        targetIds_.push_back({1, ss::kAllOutputs});
        targetBox_.setSelectedId(1, juce::dontSendNotification);
    }

    void send(ss::ActionId id, double value) {
        const auto t = sendAction(model_, id, value, target_);
        if (!t.result.queued) setNotice(uikit::refusalText(t));
    }

    void nudgeBpm(double d) {
        const auto& s = lastBpm_;
        const double base = s > 0.0 ? s : bpm_.getValue();
        const double next = std::clamp(base + d, 40.0, 240.0);
        bpm_.setValue(next, juce::dontSendNotification);
        send(ss::ActionId::SetBpm, next);
    }

    void effectToggled(ss::ActionId id, juce::ToggleButton& on, juce::Slider& amount) {
        const double v = on.getToggleState() ? amount.getValue() : 0.0;
        send(id, v);
    }

    void sliderValueChanged(juce::Slider* s) override {
        if (syncing_ || s->isMouseButtonDown()) return;   // a drag is sent on release
        sendSlider(s);
    }
    void sliderDragEnded(juce::Slider* s) override { if (!syncing_) sendSlider(s); }

    void sendSlider(juce::Slider* s) {
        if (s == &bpm_) send(ss::ActionId::SetBpm, s->getValue());
        else if (s == &speed_) send(ss::ActionId::Speed, s->getValue());
        else if (s == &hue_) send(ss::ActionId::ColorHue, s->getValue());
        else if (s == &sat_) send(ss::ActionId::ColorSaturation, s->getValue());
        else if (s == &prismAmount_) { if (prismOn_.getToggleState()) send(ss::ActionId::EffectPrismMirror, s->getValue()); }
        else if (s == &bloomAmount_) { if (bloomOn_.getToggleState()) send(ss::ActionId::EffectBloomPulse, s->getValue()); }
    }

    void syncTargets(const UiSnapshot& s) {
        juce::String key;
        for (const auto& o : s.outputs) key << juce::String(o.id) << "|" << o.label << ";";
        if (key == outputsKey_) return;
        outputsKey_ = key;
        targetBox_.clear(juce::dontSendNotification);
        targetIds_.clear();
        targetBox_.addItem("All visual outputs", 1);
        targetIds_.push_back({1, ss::kAllOutputs});
        int next = 2;
        bool found = target_ == ss::kAllOutputs;
        for (const auto& o : s.outputs) {
            const juce::String label = o.label.empty() ? juce::String("Output " + std::to_string(o.id)) : juce::String(o.label);
            targetBox_.addItem(label, next);
            targetIds_.push_back({next, o.id});
            if (o.id == target_) { found = true; targetBox_.setSelectedId(next, juce::dontSendNotification); }
            ++next;
        }
        if (!found) {
            target_ = ss::kAllOutputs;
            targetBox_.setSelectedId(1, juce::dontSendNotification);
        }
    }

    void syncTempo(const UiSnapshot& s) {
        const auto& f = s.features;
        const auto& sh = s.show;
        const double shown = sh.tempo.valid() ? sh.tempo.bpm : 0.0;
        lastBpm_ = shown;
        std::string info = live::tempoText(sh.tempo);
        if (sh.tempo.valid()) {
            info += ", " + live::tempoSourceWord(sh.tempo.source) + ", " + std::to_string(live::confidencePercent(f.tempo.confidence)) + "% confidence";
        }
        info += ". " + std::string(live::keyText(sh.key));
        if (sh.key.valid()) info += ", " + live::tempoSourceWord(sh.key.source);
        tempoInfo_.setText(juce::String(info), juce::dontSendNotification);
        releaseBpm_.setEnabled(sh.tempoOverride);
        if (!bpm_.isMouseButtonDown() && shown > 0.0) {
            syncing_ = true;
            bpm_.setValue(shown, juce::dontSendNotification);
            syncing_ = false;
        }
        if (!sh.keyOverride) keyBox_.setSelectedId(1, juce::dontSendNotification);
    }

    void mode(ss::ShowMode m) {
        modeAuto_.setToggleState(m == ss::ShowMode::Auto, juce::dontSendNotification);
        modeProg_.setToggleState(m == ss::ShowMode::Programmed, juce::dontSendNotification);
        modeHybrid_.setToggleState(m == ss::ShowMode::Hybrid, juce::dontSendNotification);
    }

    void syncEffects(const ss::model::SceneParams& p) {
        if (prismAmount_.isMouseButtonDown() || bloomAmount_.isMouseButtonDown()) return;
        syncing_ = true;
        prismOn_.setToggleState(p.effectAmount[0] > 0.f, juce::dontSendNotification);
        bloomOn_.setToggleState(p.effectAmount[1] > 0.f, juce::dontSendNotification);
        if (p.effectAmount[0] > 0.f) prismAmount_.setValue(p.effectAmount[0], juce::dontSendNotification);
        if (p.effectAmount[1] > 0.f) bloomAmount_.setValue(p.effectAmount[1], juce::dontSendNotification);
        syncing_ = false;
    }

    // ---- layout ----------------------------------------------------------------------------------
    int layoutWide(int width, int height) {
        const int leftW = 344, rightX = 360, rightW = std::max(432, width - rightX - 8);
        targetBox_.setBounds(8, 8, leftW, 32);
        preview_.setBounds(8, 44, leftW, 236);
        tempoInfo_.setBounds(8, 284, leftW, 22);
        auto tempo = juce::Rectangle<int>(8, 310, leftW, 44);
        uikit::placeRow(tempo, {{&tap_, 52}, {&nudgeDown_, 32}, {&bpm_, 84}, {&nudgeUp_, 32}, {&releaseBpm_, 64}, {&keyBox_, 0}});
        arc_.setBounds(8, 362, leftW, 40);
        notice_.setBounds(8, 408, leftW, 40);

        uikit::placeRow(juce::Rectangle<int>(rightX, 8, rightW, 32), {{&modeAuto_, 72}, {&modeProg_, 96}, {&modeHybrid_, 80}, {&cuePrev_, 34}, {&cueLabel_, 0}, {&cueNext_, 34}});
        const int padW = (rightW - 3 * 8) / 4;
        for (int i = 0; i < 8; ++i) {
            const int col = i % 4, row = i / 4;
            pads_[i]->setBounds(rightX + col * (padW + 8), 48 + row * 72, padW, 64);
        }
        uikit::placeRow(juce::Rectangle<int>(rightX, 192, rightW, 32), {{&bankPrev_, 32}, {&bankLabel_, 0}, {&bankNext_, 32}});
        placeSlider(juce::Rectangle<int>(rightX, 232, rightW, 28), "Speed", speed_);
        placeSlider(juce::Rectangle<int>(rightX, 262, rightW, 28), "Hue", hue_);
        placeSlider(juce::Rectangle<int>(rightX, 292, rightW, 28), "Saturation", sat_);
        uikit::placeRow(juce::Rectangle<int>(rightX, 332, rightW, 32), {{&prismOn_, 130}, {&prismAmount_, 0}});
        uikit::placeRow(juce::Rectangle<int>(rightX, 368, rightW, 32), {{&bloomOn_, 130}, {&bloomAmount_, 0}});
        uikit::placeRow(juce::Rectangle<int>(rightX, 408, rightW, 40), {{&undo_, 56}, {&redo_, 56}, {&storeA_, 0}, {&recallA_, 0}, {&storeB_, 0}, {&recallB_, 0}});
        return std::max(height, 456);
    }

    int layoutNarrow(int width) {
        int y = 0;
        auto row = [&](int h, std::initializer_list<std::pair<juce::Component*, int>> items) {
            uikit::placeRow(juce::Rectangle<int>(0, y, width, h), items);
            y += h + 6;
        };
        targetBox_.setBounds(0, y, width, 32); y += 38;
        preview_.setBounds(0, y, std::min(width, 320), 180); y += 186;
        tempoInfo_.setBounds(0, y, width, 22); y += 24;
        row(44, {{&tap_, 52}, {&nudgeDown_, 32}, {&bpm_, 84}, {&nudgeUp_, 32}, {&releaseBpm_, 64}, {&keyBox_, 0}});
        arc_.setBounds(0, y, width, 40); y += 46;
        row(32, {{&modeAuto_, 72}, {&modeProg_, 96}, {&modeHybrid_, 80}, {&cuePrev_, 34}, {&cueLabel_, 0}, {&cueNext_, 34}});
        const int padW = (width - 3 * 8) / 4;
        for (int i = 0; i < 8; ++i) pads_[i]->setBounds((i % 4) * (padW + 8), y + (i / 4) * 72, padW, 64);
        y += 144;
        row(32, {{&bankPrev_, 32}, {&bankLabel_, 0}, {&bankNext_, 32}});
        placeSlider(juce::Rectangle<int>(0, y, width, 28), "Speed", speed_); y += 30;
        placeSlider(juce::Rectangle<int>(0, y, width, 28), "Hue", hue_); y += 30;
        placeSlider(juce::Rectangle<int>(0, y, width, 28), "Saturation", sat_); y += 34;
        row(32, {{&prismOn_, 130}, {&prismAmount_, 0}});
        row(32, {{&bloomOn_, 130}, {&bloomAmount_, 0}});
        row(40, {{&undo_, 56}, {&redo_, 56}, {&storeA_, 0}, {&recallA_, 0}, {&storeB_, 0}, {&recallB_, 0}});
        notice_.setBounds(0, y, width, 40); y += 46;
        return y;
    }

    static void placeSlider(juce::Rectangle<int> r, const char* label, juce::Slider& s) {
        (void)label;
        s.setBounds(r.withTrimmedLeft(96));
    }

    UiModel& model_;
    LivePreview preview_;
    juce::ComboBox targetBox_;
    std::vector<std::pair<int, OutputId>> targetIds_;
    OutputId target_ = ss::kAllOutputs;
    juce::String outputsKey_;
    juce::TextButton tap_, nudgeDown_, nudgeUp_, releaseBpm_;
    juce::Slider bpm_;
    juce::ComboBox keyBox_;
    juce::Label tempoInfo_;
    EnergyArcMeter arc_;
    juce::TextButton modeAuto_, modeProg_, modeHybrid_, cuePrev_, cueNext_;
    juce::Label cueLabel_;
    std::unique_ptr<ScenePad> pads_[8];
    juce::TextButton bankPrev_, bankNext_;
    juce::Label bankLabel_;
    juce::Slider speed_, hue_, sat_;
    juce::ToggleButton prismOn_, bloomOn_;   // ToggleButton keeps the on/off state without a radio group
    juce::Slider prismAmount_, bloomAmount_;
    juce::TextButton undo_, redo_, storeA_, recallA_, storeB_, recallB_;
    juce::Label notice_;
    double lastBpm_ = 0.0;
    bool syncing_ = false, compact_ = false;
};

LivePanel::LivePanel(UiModel& model) : PanelBase(model, "Live") {
    content_ = std::make_unique<Content>(model);
    viewport_.setViewedComponent(content_.get(), false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
    setWantsKeyboardFocus(true);
}

LivePanel::~LivePanel() {
    viewport_.setViewedComponent(nullptr, false);
    content_.reset();
}

void LivePanel::paint(juce::Graphics& g) { g.fillAll(theme::bgNight()); }

void LivePanel::resized() {
    const bool compact = theme::isCompact(getWidth(), getHeight());
    content_->setCompact(compact);
    viewport_.setBounds(getLocalBounds());
    const int scrollW = compact ? 14 : 0;
    const int w = getWidth() - scrollW;
    const int h = content_->layout(w, getHeight());
    content_->setSize(w, h);
}

bool LivePanel::keyPressed(const juce::KeyPress& key) { return live::handleShortcut(model_, key); }

OutputId LivePanel::target() const { return content_->target(); }

juce::Component* LivePanel::control(const juce::String& componentId) { return content_->control(componentId); }

juce::String LivePanel::noticeText() const { return content_->notice(); }

void LivePanel::uiSnapshotChanged(const UiSnapshot& s) {
    content_->apply(s);
}

void LivePanel::uiActionOutcome(const ActionOutcome& o) {
    if (!o.accepted && !o.reason.empty()) content_->setNotice(juce::String(o.reason));
}

}  // namespace ss::app
