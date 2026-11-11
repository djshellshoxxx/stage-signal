// Setup tab (UJ-01 wizard as one scrolling, fully keyboard-operable page): displays, audio+gain, camera, MIDI ports,
// lighting interface, recording folder, benchmark. Nothing here arms DMX, starts recording or broadcasts.
#include "ui/pages.h"
#include "ui/identify.h"
#include "platform/windows/platform.h"
#include <future>

namespace ss { namespace ui {
namespace {
class SetupPage : public Page {
 public:
  SetupPage() : Page("Setup") {}
  void build() override {
    int y = 4;
    y = heading("Setup progress (steps 1-8; DMX, recording and broadcast stay off)", y);
    lblSteps_ = label("", 8, y, 760, 170); y += 176;
    button("Save setup", 8, y, 110, 32, [this] { Status s = ctl().saveSetup(); postNotice(s.ok() ? "Setup saved." : "Setup not saved: " + s.detail, !s.ok()); });
    y += 44;
    // 1 displays
    y = heading("1. Displays: identify and assign roles", y);
    label("Identify shows a numbered label on every display for a few seconds (operator-triggered; nothing else changes).", 8, y, 760, 20); y += 24;
    button("Identify displays", 8, y, 140, 32, [] { identifyDisplays(); });
    button("Refresh displays", 154, y, 140, 32, [this] { ctl().refreshDisplays(); });
    y += 40;
    tblRoutes_ = table(8, y, 760, 110, {{"Output", 110}, {"Role", 80}, {"Display", 200}, {"State", 120}, {"Note", 240}}); y += 118;
    label("Output:", 8, y + 4, 60, 22); cbOut_ = combo(70, y, 150, nullptr, 160);
    label("Display:", 228, y + 4, 60, 22); cbDisp_ = combo(290, y, 300, nullptr, 180); y += 36;
    button("Assign", 8, y, 90, 32, [this] { assign(); }, "Assign the chosen output to the chosen display");
    button("Unassign", 104, y, 90, 32, [this] { int o = comboSel(cbOut_); if (o >= 0 && o < (int)outIds_.size()) { Status s = ctl().unassignOutput(outIds_[(size_t)o]); if (!s.ok()) postNotice(s.detail, true); } });
    button("Set as controls display", 200, y, 180, 32, [this] { int d = comboSel(cbDisp_); if (d >= 0 && d < (int)dispIds_.size()) { Status s = ctl().setControlsDisplay(dispIds_[(size_t)d]); postNotice(s.ok() ? "Controls display set." : s.detail, !s.ok()); } }, "Operator panels stay on this display; audience windows never cover it");
    y += 38;
    chkDiag_ = check("Show diagnostics (reference border) on the selected audience output - off by default", 8, y, 760, [this](bool on) { int o = comboSel(cbOut_); if (o >= 0 && o < (int)outIds_.size()) ctl().setOutputDiagnostics(outIds_[(size_t)o], on); });
    y += 36;
    // 2 audio
    y = heading("2. Audio input and gain staging", y);
    label("Device:", 8, y + 4, 60, 22); cbAudio_ = combo(70, y, 420, nullptr, 200);
    label("Route:", 500, y + 4, 50, 22); cbRoute_ = combo(554, y, 150, nullptr, 100); comboFill(cbRoute_, {"Stereo", "Left to mono", "Right to mono"}, 0);
    y += 36;
    button("Open selected device", 8, y, 170, 32, [this] { openAudio(); });
    button("Reopen same device", 184, y, 170, 32, [this] { Status s = ctl().reopenAudio(); postNotice(s.ok() ? "Audio reopened." : "Audio: " + s.detail, !s.ok()); }, "Reopens the saved device only; never switches to another one");
    button("Close", 360, y, 80, 32, [this] { ctl().closeAudio(); });
    button("Refresh list", 446, y, 110, 32, [this] { audioSig_.clear(); });
    y += 40;
    lblAudio_ = label("", 8, y, 760, 22); y += 24;
    meter_ = mkMeter(hwnd, S(8), S(y), S(600), S(30)); track(meter_, 8, y, 600, 30); y += 36;
    lblGain_ = label("", 8, y, 760, 62); y += 66;
    chkGain_ = check("I have set the input gain on the interface/mixer (peaks below -6 dBFS, clip indicator never lit)", 8, y, 760, [this](bool on) { ctl().acknowledgeGainNote(on); }); y += 36;
    // 3 camera
    y = heading("3. Camera preview (preview only; publishing is a separate explicit choice)", y);
    label("Camera:", 8, y + 4, 60, 22); cbCam_ = combo(70, y, 340, nullptr, 160);
    button("Open preview", 416, y - 2, 110, 32, [this] { openCamera(); });
    button("Close", 532, y - 2, 80, 32, [this] { ctl().closeCamera(); });
    y += 40;
    camView_ = mkFrameView(hwnd, S(8), S(y), S(320), S(240)); track(camView_, 8, y, 320, 240);
    lblCam_ = label("", 340, y, 428, 60);
    chkPub_ = check("Publish camera to ALL audience/program outputs (explicit; off by default)", 340, y + 70, 428, [this](bool on) {
      app::CameraPublication p; p.published = on; ctl().publishCamera(p); postNotice(on ? "Camera is now published to all outputs." : "Camera publication off.", false); });
    y += 250;
    // 4 midi
    y = heading("4. MIDI ports (mappings and learn are on the MIDI / Lighting tab)", y);
    label("Input:", 8, y + 4, 50, 22); cbMidiIn_ = combo(60, y, 300, nullptr, 160);
    button("Select input", 366, y - 2, 110, 32, [this] { int i = comboSel(cbMidiIn_); if (i >= 0 && i < (int)midiIn_.size()) { Status s = ctl().selectMidiInput(midiIn_[(size_t)i]); postNotice(s.ok() ? "MIDI input selected." : s.detail, !s.ok()); } });
    y += 36;
    label("Output:", 8, y + 4, 50, 22); cbMidiOut_ = combo(60, y, 300, nullptr, 160);
    button("Select output", 366, y - 2, 110, 32, [this] { int i = comboSel(cbMidiOut_); if (i >= 0 && i < (int)midiOut_.size()) { Status s = ctl().selectMidiOutput(midiOut_[(size_t)i]); postNotice(s.ok() ? "MIDI output selected." : s.detail, !s.ok()); } });
    button("Reconnect MIDI", 482, y - 2, 130, 32, [this] { Status s = ctl().reconnectMidi(); postNotice(s.ok() ? "MIDI reconnected." : s.detail, !s.ok()); });
    y += 40;
    // 5 lighting
    y = heading("5. Lighting interface (setup never transmits; arming is on the MIDI / Lighting tab)", y);
    label("Protocol:", 8, y + 4, 70, 22); cbProto_ = combo(80, y, 130, nullptr, 100); comboFill(cbProto_, {"Art-Net", "sACN"}, 0);
    label("Host:", 220, y + 4, 40, 22); edHost_ = edit("", 262, y, 160, 28);
    label("NIC:", 430, y + 4, 36, 22); cbNic_ = combo(468, y, 240, nullptr, 140);
    y += 36;
    chkMc_ = check("sACN multicast", 8, y, 150, nullptr); chkBc_ = check("Allow broadcast destination (explicit opt-in)", 170, y, 360, nullptr);
    button("Apply interface", 540, y - 3, 130, 32, [this] { applyLighting(); }); y += 40;
    // 6 recording
    y = heading("6. Recording folder", y);
    edRec_ = edit("", 8, y, 560, 28);
    button("Browse...", 574, y - 2, 90, 32, [this] { std::wstring p; if (pickFolder(A.mainWnd, L"Recording folder", p)) SetWindowTextW(edRec_, p.c_str()); });
    button("Set folder", 670, y - 2, 98, 32, [this] { Status s = ctl().setRecordingFolder(getText(edRec_)); postNotice(s.ok() ? "Recording folder set." : s.detail, !s.ok()); });
    y += 44;
    // 7 benchmark
    y = heading("7. Benchmark (mock sinks only; never transmits, broadcasts or records live)", y);
    btnBench_ = button("Run benchmark (about 1 minute)", 8, y, 240, 32, [this] { runBench(); });
    lblBench_ = label("Not run yet. The result decides which features are admitted on this machine (spec section 7).", 256, y + 4, 512, 44);
    y += 56;
    setContentHeight(y);
    SendMessageW(chkGain_, BM_SETCHECK, BST_UNCHECKED, 0);
  }
  void activated() override { audioSig_.clear(); lightFilled_ = false; }
  void refresh() override {
    auto ss = ctl().setupState(); auto st = ctl().status();
    std::string steps; for (size_t i = 0; i < ss.steps.size(); ++i) steps += fmt("%d. [%s] %s%s - %s\r\n", (int)i + 1, ss.steps[i].complete ? "done" : "todo", ss.steps[i].title.c_str(), ss.steps[i].optional ? " (optional)" : "", ss.steps[i].summary.c_str());
    setTextIf(lblSteps_, steps + (ss.complete() ? "Setup complete." : "Required steps remain."));
    // routes
    auto routes = ctl().outputRoutes(); std::vector<std::vector<std::string>> rows; outIds_.clear(); std::vector<std::string> outNames;
    for (auto& r : routes) {
      const char* role = r.role == show::OutputRole::Audience ? "Audience" : r.role == show::OutputRole::Program ? "Program" : "Controls";
      rows.push_back({r.logicalId, role, r.displayName.empty() ? "(none)" : r.displayName, app::routeStateName(r.state), r.note});
      if (r.role != show::OutputRole::Controls) { outIds_.push_back(r.logicalId); outNames.push_back(r.logicalId + " (" + role + ")"); }
    }
    tableSet(tblRoutes_, rows);
    std::string osig; for (auto& s : outNames) osig += s + "|"; if (osig != outSig_) { outSig_ = osig; comboFill(cbOut_, outNames, 0); }
    auto disp = ctl().displays(); std::vector<std::string> dn; dispIds_.clear(); dispInfo_ = disp; std::string dsig;
    for (auto& d : disp) { dn.push_back(fmt("Display %d: %s (%dx%d)%s%s", d.index + 1, d.name.c_str(), d.w, d.h, d.primary ? " primary" : "", d.ambiguous ? " IDENTICAL-MODEL" : "")); dispIds_.push_back(d.stableId); dsig += dn.back() + "|"; }
    if (dsig != dispSig_) { dispSig_ = dsig; comboFill(cbDisp_, dn, 0); }
    // audio list
    if (audioSig_.empty()) {
      auto devs = ctl().audioDevices(); audio_ = devs; std::vector<std::string> n; int sel = -1; audioSig_ = "x";
      for (size_t i = 0; i < devs.size(); ++i) { n.push_back(devs[i].name + (devs[i].isDefault ? " (system default)" : "")); if (devs[i].id == ss.audioDeviceId) sel = (int)i; }
      comboFill(cbAudio_, n, sel < 0 && !n.empty() ? 0 : sel);
      if (devs.empty()) SendMessageW(cbAudio_, CB_ADDSTRING, 0, (LPARAM)L"(no capture devices found)");
    }
    const char* as[] = {"Closed", "Opening", "Running", "UNAVAILABLE", "DENIED", "DISCONNECTED"};
    setTextIf(lblAudio_, "Audio: " + (st.audioDevice.empty() ? std::string("no device selected") : st.audioDevice) + " - " + as[(int)st.audioState] + (st.audioDetail.empty() ? "" : " (" + st.audioDetail + ")"));
    meterSet(meter_, st.meterRmsDb, st.meterPeakDb, st.inputNearFullScale, st.possibleClipping);
    setTextIf(lblGain_, ss.gainNote);
    // camera
    if (camSig_.empty()) { auto cams = ctl().cameras(); cams_ = cams; std::vector<std::string> n; for (auto& c : cams) n.push_back(c.name); comboFill(cbCam_, n, n.empty() ? -1 : 0); camSig_ = "x"; if (cams.empty()) SendMessageW(cbCam_, CB_ADDSTRING, 0, (LPARAM)L"(no cameras found)"); }
    const char* cs[] = {"Closed", "Opening", "Live", "Frozen", "LOST", "ERROR"};
    setTextIf(lblCam_, std::string("Camera status: ") + cs[(int)st.cameraState] + (st.cameraName.empty() ? "" : " (" + st.cameraName + ")") + (st.cameraPublished ? "\r\nPUBLISHED to outputs" : "\r\nPreview only - not in any scene"));
    app::FramePtr f; if (ctl().cameraPreviewFrame(f)) frameViewSet(camView_, f);
    // midi
    if (midiSig_.empty()) {
      auto ps = ctl().midiPorts(); midiIn_.clear(); midiOut_.clear(); std::vector<std::string> a, b;
      for (auto& p : ps) { if (p.input) { midiIn_.push_back(p); a.push_back(p.name); } else { midiOut_.push_back(p); b.push_back(p.name); } }
      comboFill(cbMidiIn_, a, a.empty() ? -1 : 0); comboFill(cbMidiOut_, b, b.empty() ? -1 : 0); midiSig_ = "x";
      if (a.empty()) SendMessageW(cbMidiIn_, CB_ADDSTRING, 0, (LPARAM)L"(no MIDI inputs)");
      if (b.empty()) SendMessageW(cbMidiOut_, CB_ADDSTRING, 0, (LPARAM)L"(no MIDI outputs)");
    }
    if (!lightFilled_) {
      lightFilled_ = true; auto nics = win::listNics(); std::vector<std::string> n = {"(system default)"}; nicAddr_ = {""};
      for (auto& x : nics) { n.push_back(x.name + " " + x.ipv4); nicAddr_.push_back(x.ipv4); }
      comboFill(cbNic_, n, 0);
      SetWindowTextW(edRec_, W(ss.recordingFolder).c_str()); SetWindowTextW(edHost_, W(ss.lighting.host).c_str());
      SendMessageW(cbProto_, CB_SETCURSEL, ss.lighting.protocol == LightProtocol::Sacn ? 1 : 0, 0);
    }
    if (bench_.valid() && bench_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      app::BenchmarkOutcome o = bench_.get();
      setTextIf(lblBench_, o.status.ok() ? "Benchmark finished: tier " + o.profile.tier + (o.result.qualifying ? " (qualifying)" : " (non-qualifying)") + (o.jsonPath.empty() ? "" : "; result: " + o.jsonPath) : "Benchmark failed: " + o.status.detail);
      setEnabled(btnBench_, true);
    }
  }
 private:
  void assign() {
    int o = comboSel(cbOut_), d = comboSel(cbDisp_);
    if (o < 0 || o >= (int)outIds_.size() || d < 0 || d >= (int)dispInfo_.size()) { postNotice("Choose an output and a display first.", true); return; }
    Status s = ctl().assignOutput(outIds_[(size_t)o], dispInfo_[(size_t)d]);
    postNotice(s.ok() ? "Assigned " + outIds_[(size_t)o] + " to " + dispInfo_[(size_t)d].name : "Not assigned: " + s.detail, !s.ok());
  }
  void openAudio() {
    int i = comboSel(cbAudio_); if (i < 0 || i >= (int)audio_.size()) { postNotice("Choose an audio device first.", true); return; }
    AudioOpenParams p; p.deviceId = audio_[(size_t)i].id; p.channelRoute = std::max(0, comboSel(cbRoute_));
    Status s = ctl().selectAudio(p); postNotice(s.ok() ? "Audio opened: " + audio_[(size_t)i].name : "Audio not opened: " + s.detail, !s.ok());
  }
  void openCamera() {
    int i = comboSel(cbCam_); if (i < 0 || i >= (int)cams_.size()) { postNotice("No camera selected.", true); return; }
    Status s = ctl().selectCamera(cams_[(size_t)i]); postNotice(s.ok() ? "Camera preview opened (not published)." : "Camera: " + s.detail, !s.ok());
  }
  void applyLighting() {
    app::LightingInterface li; li.protocol = comboSel(cbProto_) == 1 ? LightProtocol::Sacn : LightProtocol::ArtNet; li.host = getText(edHost_);
    int n = comboSel(cbNic_); if (n > 0 && n < (int)nicAddr_.size()) li.nicAddr = nicAddr_[(size_t)n];
    li.sacnMulticast = SendMessageW(chkMc_, BM_GETCHECK, 0, 0) == BST_CHECKED; li.allowBroadcast = SendMessageW(chkBc_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    Status s = ctl().setLightingInterface(li); postNotice(s.ok() ? "Lighting interface saved (still disarmed)." : "Lighting interface: " + s.detail, !s.ok());
  }
  void runBench() {
    if (bench_.valid()) return; app::BenchmarkRequest rq; setEnabled(btnBench_, false);
    setTextIf(lblBench_, "Benchmark running (mock sinks; the UI stays responsive)...");
    bench_ = ctl().runBenchmark(rq);
  }
  HWND lblSteps_ = nullptr, tblRoutes_ = nullptr, cbOut_ = nullptr, cbDisp_ = nullptr, chkDiag_ = nullptr, cbAudio_ = nullptr, cbRoute_ = nullptr, lblAudio_ = nullptr, meter_ = nullptr, lblGain_ = nullptr,
       chkGain_ = nullptr, cbCam_ = nullptr, camView_ = nullptr, lblCam_ = nullptr, chkPub_ = nullptr, cbMidiIn_ = nullptr, cbMidiOut_ = nullptr, cbProto_ = nullptr, edHost_ = nullptr, cbNic_ = nullptr,
       chkMc_ = nullptr, chkBc_ = nullptr, edRec_ = nullptr, btnBench_ = nullptr, lblBench_ = nullptr;
  std::vector<std::string> outIds_, dispIds_, nicAddr_; std::vector<DisplayInfo> dispInfo_; std::vector<AudioDeviceInfo> audio_; std::vector<CameraInfo> cams_;
  std::vector<MidiPortInfo> midiIn_, midiOut_; std::string outSig_, dispSig_, audioSig_, camSig_, midiSig_; bool lightFilled_ = false;
  std::future<app::BenchmarkOutcome> bench_;
};
} // namespace
std::unique_ptr<Page> makeSetupPage() { return std::unique_ptr<Page>(new SetupPage()); }
}} // namespace ss::ui
