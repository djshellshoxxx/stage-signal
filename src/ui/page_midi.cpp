// MIDI / Lighting tab: mapping table + learn, clock/feedback opt-in with alarm state; lighting universe/patch/profile view,
// explicit Arm/Disarm (confirmed), dry-run indicator. Editing here never transmits DMX.
#include "ui/pages.h"
#include "lighting/fixture.h"
#include "nlohmann/json.hpp"
#include <set>
#include <algorithm>

namespace ss { namespace ui {
namespace {
using nlohmann::json;
class MidiLightPage : public Page {
 public:
  MidiLightPage() : Page("MIDI / Lighting") {}
  void build() override {
    int y = 4;
    y = heading("MIDI", y);
    lblMidi_ = label("", 8, y, 760, 44); y += 48;
    tblMap_ = table(8, y, 760, 120, {{"Id", 70}, {"Device", 150}, {"Ch", 40}, {"Type", 60}, {"No.", 50}, {"Action", 150}, {"Mode", 110}, {"Status", 130}}); y += 128;
    button("Delete selected mapping", 8, y, 200, 32, [this] { int i = tableSel(tblMap_); if (i >= 0 && i < (int)ids_.size()) { ctl().midiMappings().remove(ids_[(size_t)i]); } else postNotice("Select a mapping first.", true); });
    y += 40;
    label("Learn: choose an action, press Learn, then move/press the control on the MIDI device.", 8, y, 760, 22, true); y += 26;
    label("Action:", 8, y + 4, 50, 22); cbAct_ = combo(60, y, 190, nullptr, 260);
    label("Mode:", 260, y + 4, 44, 22); cbMode_ = combo(306, y, 190, nullptr, 130); comboFill(cbMode_, {"Absolute", "Relative (two's comp)", "Relative (binary offset)", "Relative (sign magnitude)"}, 0);
    chkMode_ = check("I confirm the encoder mode", 506, y, 262, nullptr); y += 36;
    std::vector<std::string> an; for (int i = 1; i < (int)ActionId::Count_; ++i) { actIds_.push_back((ActionId)i); an.push_back(midiActionName((ActionId)i)); } comboFill(cbAct_, an, 1);
    button("Learn", 8, y, 90, 32, [this] { learn(); });
    button("Commit learned", 104, y, 130, 32, [this] { std::string why; std::string id = ctl().midiMappings().commitLearn(false, &why); postNotice(id.empty() ? "Not added: " + why : "Mapping added: " + id, id.empty()); });
    button("Cancel learn", 240, y, 110, 32, [this] { ctl().midiMappings().cancelLearn(); });
    lblLearn_ = label("", 360, y + 4, 408, 22); y += 42;
    chkClock_ = check("Send MIDI clock (24 ppqn from the effective BPM) - opt-in", 8, y, 500, [this](bool on) { ctl().setMidiClockEnabled(on); }); y += 28;
    chkFb_ = check("MIDI feedback to the output port (blackout/pause state as notes 36-38) - explicit opt-in, loop-protected", 8, y, 760, [this](bool on) { feedback(on); }); y += 28;
    lblAlarm_ = label("", 8, y, 760, 44, true); y += 52;

    y = heading("Lighting", y);
    lblLight_ = label("", 8, y, 760, 44, true); y += 48;
    btnArm_ = button("Arm DMX output...", 8, y, 160, 34, [this] { arm(); }, "Explicit confirmation required; nothing transmits until armed");
    button("Disarm", 174, y, 100, 34, [] { act(ActionId::DisarmLighting); });
    label("Blackout and Emergency Blackout (bottom strip, F8) override every cue. They cannot guarantee a dark rig after a cable/network loss: set node/fixture loss behaviour.", 282, y - 2, 486, 40);
    y += 46;
    tblPatch_ = table(8, y, 760, 120, {{"Fixture", 110}, {"Profile", 150}, {"Universe", 70}, {"Address", 70}, {"Channels", 70}, {"Label", 110}, {"Check", 170}}); y += 128;
    label("Id:", 8, y + 4, 24, 22); edId_ = edit("", 34, y, 90, 28);
    label("Profile:", 132, y + 4, 54, 22); cbProf_ = combo(188, y, 190, [this](int) { showProfile(); }, 200);
    label("Univ.:", 386, y + 4, 44, 22); edUni_ = edit("1", 432, y, 50, 28, ES_NUMBER | ES_RIGHT);
    label("Addr:", 490, y + 4, 40, 22); edAddr_ = edit("1", 532, y, 50, 28, ES_NUMBER | ES_RIGHT);
    button("Add", 590, y - 2, 60, 32, [this] { addFixture(); }); button("Remove selected", 656, y - 2, 112, 32, [this] { removeFixture(); });
    y += 40;
    auto& lib = ProfileLibrary::builtin(); profIds_ = lib.ids(); comboFill(cbProf_, profIds_, 0);
    label("Fixture profile (data only; built-in profiles are generic and not hardware-qualified):", 8, y, 760, 22); y += 24;
    edProf_ = edit("", 8, y, 760, 100, ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL); y += 108;
    setContentHeight(y);
    showProfile();
  }
  void activated() override { sig_.clear(); }
  void refresh() override {
    auto st = ctl().status(); auto& mt = ctl().midiMappings();
    setTextIf(lblMidi_, fmt("MIDI input: %s | output: %s | %zu mapping(s), %zu unavailable (device lost)", st.midiInputAlive ? "connected" : "NOT connected", st.midiOutputAlive ? "connected" : "NOT connected", st.midiMappings, st.midiMappingsUnavailable) +
      "\r\nA lost device marks its mappings unavailable; after reconnecting no stale actions are replayed.");
    auto maps = mt.list(); ids_.clear(); std::vector<std::vector<std::string>> rows;
    const char* ms[] = {"Active", "DEVICE UNAVAILABLE", "Needs device confirm", "Needs mode confirm"};
    for (auto& m : maps) { ids_.push_back(m.id); rows.push_back({m.id, m.deviceName.empty() ? m.deviceId : m.deviceName, m.channel < 0 ? "any" : std::to_string(m.channel + 1), midiTypeName(m.type), std::to_string(m.number), midiActionName(m.action), midiModeName(m.mode), ms[(int)mt.status(m.id)]}); }
    tableSet(tblMap_, rows);
    LearnInfo li = mt.learnInfo();
    setTextIf(lblLearn_, li.active ? (li.captured ? fmt("Captured %s #%d on channel %d (value %d). Press Commit.", midiTypeName(li.type), li.number, li.channel + 1, li.rawValue) : "Learning: waiting for a MIDI message...") : "Not learning.");
    std::string alarm; for (auto& w : st.warnings) if (w.id.compare(0, 4, "midi") == 0) alarm += (w.critical ? "ALARM: " : "Warning: ") + w.text + " ";
    setTextIf(lblAlarm_, alarm.empty() ? "MIDI feedback state: no loop alarm. Feedback routes are off unless enabled above." : alarm);
    // lighting
    bool armed = ctl().lightingArmed();
    setTextIf(lblLight_, armed ? fmt("LIGHTING: ARMED - %llu datagram(s) sent to the network", (unsigned long long)ctl().lightingDatagramsSent())
                               : fmt("LIGHTING: DISARMED (DRY-RUN). Nothing is transmitted; %llu datagram(s) previewed internally, %llu sent.", (unsigned long long)ctl().lightingDatagramsPreviewed(), (unsigned long long)ctl().lightingDatagramsSent()));
    setEnabled(btnArm_, !armed);
    auto doc = ctl().document(); if (!doc) return;
    std::vector<std::vector<std::string>> pr; fixIds_.clear(); std::vector<json> js;
    for (auto& s : doc->patch) { json j = json::parse(s, nullptr, false); js.push_back(j); }
    for (size_t i = 0; i < js.size(); ++i) {
      json& j = js[i]; if (!j.is_object()) { pr.push_back({"?", "?", "", "", "", "", "INVALID entry"}); fixIds_.push_back(""); continue; }
      std::string id = j.value("id", ""), prof = j.value("profile", ""); int u = j.value("universe", 0), a = j.value("address", 1);
      const FixtureProfile* fp = ProfileLibrary::builtin().find(prof); int n = fp ? fp->footprint() : 0; std::string chk = fp ? "ok" : "UNKNOWN PROFILE";
      if (fp && a + n - 1 > 512) chk = "EXCEEDS 512 CHANNELS";
      for (size_t k = 0; k < js.size(); ++k) if (k != i && js[k].is_object() && js[k].value("universe", -1) == u) {
        const FixtureProfile* o = ProfileLibrary::builtin().find(js[k].value("profile", "")); int oa = js[k].value("address", 1), on = o ? o->footprint() : 0;
        if (fp && o && a <= oa + on - 1 && oa <= a + n - 1) chk = "CONFLICT with " + js[k].value("id", ""); }
      pr.push_back({id, prof, std::to_string(u), std::to_string(a), std::to_string(n), j.value("label", ""), chk}); fixIds_.push_back(id);
    }
    tableSet(tblPatch_, pr);
  }
 private:
  void learn() {
    int a = comboSel(cbAct_); if (a < 0) return; auto ins = ctl().midiPorts(); LearnRequest r;
    for (auto& p : ins) if (p.input) { r.deviceId = p.stableId; r.deviceName = p.name; break; }
    if (r.deviceId.empty()) { postNotice("No MIDI input port is available: select one in Setup first.", true); return; }
    r.action = actIds_[(size_t)a]; int mi = std::max(0, comboSel(cbMode_)); r.mode = (MidiMode)mi; r.modeExplicit = SendMessageW(chkMode_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    Status s = ctl().midiMappings().beginLearn(r); postNotice(s.ok() ? "Learning: operate the control on the device." : "Learn: " + s.detail, !s.ok());
  }
  void feedback(bool on) {
    std::vector<app::MidiFeedbackRule> rules;
    if (on) { ActionId ids[3] = {ActionId::VisualBlackout, ActionId::EmergencyBlackout, ActionId::PauseAutomation};
      for (int i = 0; i < 3; ++i) { app::MidiFeedbackRule r; r.action = ids[i]; r.type = MidiMsgType::Note; r.channel = 0; r.number = 36 + i; rules.push_back(r); } }
    ctl().setMidiFeedback(rules);
  }
  void arm() {
    auto doc = ctl().document(); size_t n = doc ? doc->patch.size() : 0;
    if (msgBox(A.mainWnd, fmt("Arm the DMX output? StageSignal will begin transmitting lighting data for %zu patched fixture(s) on the configured interface.\n\nBlackout (F8) stays available. Continue?", n), "Arm lighting", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) act(ActionId::ArmLighting);
  }
  void showProfile() {
    int i = comboSel(cbProf_); if (i < 0 || i >= (int)profIds_.size()) return; const FixtureProfile* p = ProfileLibrary::builtin().find(profIds_[(size_t)i]); if (!p) return;
    std::string t = p->name + " - " + fmt("%d channel(s)", p->channelCount) + (p->qualified ? ", hardware-qualified" : ", NOT hardware-qualified") + "\r\n";
    for (auto& c : p->channels) t += fmt("  +%d  %s  (width %d, default %u, blackout %u)\r\n", c.offset + 1, c.name.c_str(), c.width, c.defaultValue, c.blackout());
    SetWindowTextW(edProf_, W(t).c_str());
  }
  void addFixture() {
    std::string id = getText(edId_); int pi = comboSel(cbProf_); if (id.empty() || pi < 0) { postNotice("Enter a fixture id and choose a profile.", true); return; }
    json j; j["id"] = id; j["profile"] = profIds_[(size_t)pi]; j["label"] = id; j["universe"] = atoi(getText(edUni_).c_str()); j["address"] = atoi(getText(edAddr_).c_str());
    applyDocEdit([&](show::ShowDocument& d) { d.patch.push_back(j.dump()); });
  }
  void removeFixture() {
    int i = tableSel(tblPatch_); if (i < 0) { postNotice("Select a fixture first.", true); return; }
    applyDocEdit([&](show::ShowDocument& d) { if (i < (int)d.patch.size()) d.patch.erase(d.patch.begin() + i); });
  }
  HWND lblMidi_ = nullptr, tblMap_ = nullptr, cbAct_ = nullptr, cbMode_ = nullptr, chkMode_ = nullptr, lblLearn_ = nullptr, chkClock_ = nullptr, chkFb_ = nullptr, lblAlarm_ = nullptr,
       lblLight_ = nullptr, btnArm_ = nullptr, tblPatch_ = nullptr, edId_ = nullptr, cbProf_ = nullptr, edUni_ = nullptr, edAddr_ = nullptr, edProf_ = nullptr;
  std::vector<std::string> ids_, profIds_, fixIds_; std::vector<ActionId> actIds_; std::string sig_;
};
} // namespace
std::unique_ptr<Page> makeMidiLightPage() { return std::unique_ptr<Page>(new MidiLightPage()); }
}} // namespace ss::ui
