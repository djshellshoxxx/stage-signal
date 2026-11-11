// Live tab (spec §11): 8 scene pads with named banks, always-visible target selector, mode selector, tap/manual BPM and key,
// cue list navigation and a low-rate preview. Pads are plain actions; editing fields here never reaches audience outputs.
#include "ui/pages.h"
#include <cmath>

namespace ss { namespace ui {

void applyDocEdit(const std::function<void(show::ShowDocument&)>& edit) {
  auto cur = A.ctl->document(); if (!cur) return;
  show::ShowDocument d = *cur; edit(d);
  auto f = A.ctl->applyDocument(d); Status s = waitStatus(f);
  if (!s.ok()) postNotice("Edit rejected (current show unchanged): " + s.detail, true);
}
std::vector<std::string> sceneNames(const show::ShowDocument& d) {
  std::vector<std::string> v; for (auto& s : d.scenes) v.push_back(s.name.empty() ? s.id : s.name); return v;
}

namespace {
class LivePage : public Page {
 public:
  LivePage() : Page("Live") {}
  void build() override {
    label("Controls apply to:", 8, 8, 140, 24, true);
    cbTarget_ = combo(150, 6, 290, [this](int i) { if (i >= 0 && i < (int)targetIdx_.size()) act(ActionId::SetTarget, targetIdx_[(size_t)i]); }, 160);
    tipFor(cbTarget_, "Target output for scene, brightness and text actions. 'All visual outputs' applies to every audience/program output.");
    label("Mode:", 8, 44, 50, 24, true);
    const char* names[3] = {"Auto", "Programmed", "Hybrid"}; ActionId ids[3] = {ActionId::ModeAuto, ActionId::ModeProgrammed, ActionId::ModeHybrid};
    for (int i = 0; i < 3; ++i) { ActionId id = ids[i]; btnMode_[i] = obutton(names[i], 62 + i * 96, 38, 92, 34, [id] { act(id); }, std::string("Set mode: ") + names[i]); }
    button("Prev scene", 356, 38, 84, 34, [] { act(ActionId::PrevScene); });
    bankPrev_ = button("< Bank", 8, 78, 80, 30, [this] { bankStep(-1); }, "Previous scene bank");
    lblBank_ = label("Bank A", 96, 82, 250, 22, true);
    bankNext_ = button("Bank >", 360, 78, 80, 30, [this] { bankStep(1); }, "Next scene bank");
    for (int i = 0; i < 8; ++i) {
      int r = i / 4, c = i % 4;
      pad_[i] = obutton(fmt("%d", i + 1), 8 + c * 110, 112 + r * 56, 104, 52, [i] { padHit(i); }, fmt("Scene pad %d (key %d)", i + 1, i + 1));
    }
    btnTap_ = button("Tap tempo", 8, 228, 104, 34, [] { act(ActionId::TapTempo); }, "Tap the beat; overrides the estimate until released");
    label("BPM:", 120, 234, 40, 22);
    edBpm_ = edit("120", 160, 230, 56, 28, ES_NUMBER | ES_RIGHT);
    button("Set BPM", 222, 228, 80, 34, [this] { double v = atof(getText(edBpm_).c_str()); act(ActionId::SetBpm, v); }, "Manual BPM override (20..300)");
    button("Release", 308, 228, 80, 34, [] { act(ActionId::ReleaseBpm); }, "Release tap/manual BPM; the estimate is used again");
    label("Key:", 8, 274, 36, 22);
    cbKey_ = combo(44, 270, 84, nullptr, 260);
    comboFill(cbKey_, {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 9);
    cbMinor_ = combo(134, 270, 90, nullptr, 100);
    comboFill(cbMinor_, {"major", "minor"}, 1);
    button("Set key", 232, 268, 80, 34, [this] { int pc = comboSel(cbKey_), mi = comboSel(cbMinor_); if (pc >= 0 && mi >= 0) act(ActionId::SetKey, pc + 12 * mi); }, "Manual key override");
    button("Release", 318, 268, 80, 34, [] { act(ActionId::ReleaseKey); }, "Release the manual key; the estimate is used again");
    lblTempo_ = label("", 8, 308, 432, 40);
    button("Resume: continue cue", 8, 352, 206, 34, [] { act(ActionId::ResumeContinue); }, "Resume automation at the current cue");
    button("Resume: next cue", 220, 352, 206, 34, [] { act(ActionId::ResumeNext); }, "Resume automation with the next cue");
    label("Operator preview (reduced rate)", 450, 4, 320, 20);
    preview_ = mkFrameView(hwnd, S(450), S(26), S(320), S(240)); track(preview_, 450, 26, 320, 240);
    label("Cue list", 450, 270, 100, 20, true);
    lbCue_ = listbox(450, 290, 320, 62);
    button("Prev cue", 450, 356, 100, 34, [] { act(ActionId::PrevCue); });
    button("Next cue", 556, 356, 100, 34, [] { act(ActionId::NextCue); });
    button("Trigger", 662, 356, 108, 34, [this] { int i = (int)SendMessageW(lbCue_, LB_GETCURSEL, 0, 0); if (i >= 0) act(ActionId::TriggerCue, i); }, "Trigger the selected cue");
    lblCue_ = label("", 450, 394, 320, 22);
    setContentHeight(420);
  }
  void refresh() override {
    app::AppStatus st = ctl().status(); show::ShowSnapshot sn = ctl().showSnapshot(); auto doc = ctl().document(); if (!doc) return;
    // target combo
    std::string sig; targetIdx_.clear(); std::vector<std::string> items = {"All visual outputs"}; targetIdx_.push_back(-1);
    for (size_t i = 0; i < doc->outputs.size(); ++i) if (doc->outputs[i].role != show::OutputRole::Controls) { items.push_back(doc->outputs[i].logicalId); targetIdx_.push_back((int)i); }
    for (auto& s : items) sig += s + "|";
    if (sig != targetSig_) { targetSig_ = sig; comboFill(cbTarget_, items, 0); }
    int sel = 0; for (size_t i = 0; i < targetIdx_.size(); ++i) if (targetIdx_[i] == sn.target) sel = (int)i;
    if (comboSel(cbTarget_) != sel && GetFocus() != cbTarget_) SendMessageW(cbTarget_, CB_SETCURSEL, (WPARAM)sel, 0);
    // mode
    for (int i = 0; i < 3; ++i) setBtnState(btnMode_[i], (int)sn.mode == i ? BS_ACTIVE_ : BS_NORMAL_);
    // pads
    int nb = ((int)doc->scenes.size() + 7) / 8; if (nb < 1) nb = 1;
    int bank = std::min(std::max(sn.bank, 0), nb - 1);
    setTextIf(lblBank_, fmt("Bank %c of %c (scenes %d-%d)", 'A' + bank, 'A' + nb - 1, bank * 8 + 1, std::min((bank + 1) * 8, (int)doc->scenes.size())));
    setEnabled(bankPrev_, bank > 0); setEnabled(bankNext_, bank + 1 < nb);
    for (int i = 0; i < 8; ++i) {
      int si = bank * 8 + i;
      if (si < (int)doc->scenes.size()) {
        const auto& sc = doc->scenes[(size_t)si]; bool air = si == sn.sceneIndex;
        setTextIf(pad_[i], fmt("%d: %s", i + 1, spaced(sc.name.empty() ? sc.id : sc.name).c_str()) + (air ? "\r\nON AIR" : (sc.strobe ? "\r\n(strobe)" : "")));
        setEnabled(pad_[i], true); setBtnState(pad_[i], air ? BS_ACTIVE_ : BS_NORMAL_);
      } else { setTextIf(pad_[i], fmt("%d: (empty)", i + 1)); setEnabled(pad_[i], false); setBtnState(pad_[i], BS_NORMAL_); }
    }
    // tempo / key text
    std::string t = st.bpmValid ? fmt("BPM %.1f - %s, confidence %d%%", st.bpm, st.bpmLabel.c_str(), (int)std::lround(st.bpmConfidence * 100)) : "BPM Unknown";
    t += "\r\n" + std::string("Key: ") + (st.keyValid ? st.keyName : "Unknown") + " - " + st.keyLabel;
    setTextIf(lblTempo_, t);
    // cues
    std::string cs; std::vector<std::string> cl;
    for (size_t i = 0; i < doc->cues.size(); ++i) {
      const auto& c = doc->cues[i]; std::string d = c.duration.ms > 0 ? fmt("%.1fs", c.duration.ms / 1000.0) : (c.duration.beats ? fmt("%.0f beats", c.duration.beatCount) : "hold");
      cl.push_back(std::string(sn.cueIndex == (int)i ? "> " : "  ") + fmt("%d. ", (int)i + 1) + c.id + " -> " + (c.sceneId.empty() ? "(keep)" : c.sceneId) + " [" + d + "]");
      cs += cl.back() + "\n";
    }
    if (cs != cueSig_) { cueSig_ = cs; int keep = (int)SendMessageW(lbCue_, LB_GETCURSEL, 0, 0); listFill(lbCue_, cl, keep); }
    setTextIf(lblCue_, doc->cues.empty() ? "No cues in this show" : (sn.cueIndex < 0 ? "No active cue" : fmt("Cue %d%s%s", sn.cueIndex + 1, sn.cueEnded ? " (ended, holding)" : "", sn.cueWaitingForTempo ? " (waiting for tempo)" : "")));
    // preview
    app::FramePtr f; if (ctl().previewFrame(f)) frameViewSet(preview_, f); else frameViewSet(preview_, nullptr);
  }
 private:
  void bankStep(int d) {
    auto sn = ctl().showSnapshot(); auto doc = ctl().document(); int nb = doc ? ((int)doc->scenes.size() + 7) / 8 : 1;
    int nbk = std::min(std::max(sn.bank + d, 0), std::max(nb - 1, 0)); act(ActionId::SelectBank, nbk);
  }
  static std::string spaced(std::string s) { for (auto& c : s) if (c == '_') c = ' '; return s; }
  static void padHit(int i) { auto sn = A.ctl->showSnapshot(); act(ActionId::SelectScene, sn.bank * 8 + i); }
  HWND cbTarget_ = nullptr, btnMode_[3] = {}, bankPrev_ = nullptr, lblBank_ = nullptr, bankNext_ = nullptr, pad_[8] = {};
  HWND btnTap_ = nullptr, edBpm_ = nullptr, cbKey_ = nullptr, cbMinor_ = nullptr, lblTempo_ = nullptr, preview_ = nullptr, lbCue_ = nullptr, lblCue_ = nullptr;
  std::vector<int> targetIdx_; std::string targetSig_, cueSig_;
};
} // namespace

std::unique_ptr<Page> makeLivePage() { return std::unique_ptr<Page>(new LivePage()); }

}} // namespace ss::ui
