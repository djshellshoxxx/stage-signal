// Record / Broadcast tab: explicit Start/Stop (never toggles), disk/segment/gap status, raw vs processed clip indicators,
// dynamics settings with bypass, broadcast endpoint form (stream key only via the secret store, masked), publication summary
// before Start, state-machine display and reconnect countdown. Start requires a UI-issued token (UserActionGate).
#include "ui/pages.h"
#include "broadcast/profile.h"
#include "recording/session.h"
#include <algorithm>

namespace ss { namespace ui {
namespace {
class RecordPage : public Page {
 public:
  RecordPage() : Page("Record / Broadcast") {}
  void build() override {
    int y = 4;
    y = heading("Recording (explicit Start / Stop)", y);
    label("Format:", 8, y + 4, 60, 22); cbFmt_ = combo(70, y, 300, nullptr, 120);
    comboFill(cbFmt_, {"Audio only (WAV, 24-bit)", "Video + audio (AVI, MJPEG)", "Video + audio (MKV, MJPEG)"}, 0);
    btnRecStart_ = button("Start recording", 380, y - 3, 130, 34, [this] { startRec(); });
    btnRecStop_ = button("Stop recording", 516, y - 3, 130, 34, [] { act(ActionId::StopRecording); });
    button("Add marker", 652, y - 3, 116, 34, [] { A.ctl->addMarker("Marker"); postNotice("Marker added.", false); });
    y += 40;
    lblRec_ = label("", 8, y, 760, 112); y += 116;
    label("Input (raw) level and clip indicator:", 8, y, 300, 22, true); y += 24;
    meter_ = mkMeter(hwnd, S(8), S(y), S(600), S(30)); track(meter_, 8, y, 600, 30); y += 36;
    lblProc_ = label("", 8, y, 760, 44); y += 48;

    y = heading("Recording / broadcast dynamics (compressor + limiter; on by default)", y);
    chkBypass_ = check("Bypass dynamics (recorded in metadata)", 8, y, 330, [this](bool) { applyDyn(); });
    chkComp_ = check("Compressor", 344, y, 120, [this](bool) { applyDyn(); }); chkLim_ = check("Limiter", 470, y, 100, [this](bool) { applyDyn(); });
    y += 30;
    const char* nm[9] = {"Trim dB", "Thresh dB", "Ratio", "Attack ms", "Release ms", "Knee dB", "Makeup dB", "Ceiling dB", "Lim rel ms"};
    for (int i = 0; i < 9; ++i) { int x = 8 + (i % 5) * 152, yy = y + (i / 5) * 34; label(nm[i], x, yy + 4, 66, 22); dyn_[i] = edit("", x + 68, yy, 72, 28, ES_RIGHT); }
    y += 72;
    button("Apply dynamics", 8, y, 140, 32, [this] { applyDyn(); });
    lblDyn_ = label("", 156, y + 4, 612, 22); y += 40;

    y = heading("Broadcast (explicit Start; local recording continues if the network fails)", y);
    label("Name:", 8, y + 4, 50, 22); edName_ = edit("Studio ingest", 60, y, 190, 28);
    label("Protocol:", 262, y + 4, 66, 22); cbProto_ = combo(330, y, 130, [this](int) { protoNote(); }, 120); comboFill(cbProto_, {"RTMPS", "RTMP", "SRT", "RTP"}, 0);
    lblProto_ = label("", 470, y, 298, 40); y += 36;
    label("Host:", 8, y + 4, 50, 22); edHost_ = edit("", 60, y, 250, 28);
    label("Port:", 318, y + 4, 40, 22); edPort_ = edit("0", 360, y, 60, 28, ES_NUMBER | ES_RIGHT);
    label("Path:", 428, y + 4, 40, 22); edPath_ = edit("/live", 470, y, 160, 28); y += 36;
    label("Stream key:", 8, y + 4, 90, 22); edKey_ = edit("", 100, y, 300, 28, ES_PASSWORD);
    button("Store key", 408, y - 2, 100, 32, [this] { storeKey(); }, "Stores the key in the Windows secret store (DPAPI); the box is cleared and the key is never shown, logged or exported");
    lblKey_ = label("", 514, y + 4, 254, 22); y += 36;
    label("Preset:", 8, y + 4, 54, 22); cbPre_ = combo(64, y, 300, nullptr, 120); comboFill(cbPre_, {"720p30, 2500 kb/s (default)", "BASE 800x600 @20, 1000 kb/s", "BASE 640x480 @20, 1000 kb/s"}, 0);
    label("Video kb/s:", 374, y + 4, 78, 22); edVk_ = edit("2500", 454, y, 60, 28, ES_NUMBER | ES_RIGHT); label("Audio kb/s:", 524, y + 4, 78, 22); edAk_ = edit("128", 604, y, 50, 28, ES_NUMBER | ES_RIGHT); y += 36;
    chkRe_ = check("Reconnect automatically (1/2/4/8/15 s, up to 120 s)", 8, y, 400, nullptr); SendMessageW(chkRe_, BM_SETCHECK, BST_CHECKED, 0);
    button("Apply settings", 420, y - 2, 130, 32, [this] { applySettings(); }); y += 38;
    label("What will leave this machine (shown before Start):", 8, y, 500, 22, true); y += 24;
    edSum_ = edit("", 8, y, 760, 110, ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL); y += 116;
    btnLiveStart_ = button("Start broadcast...", 8, y, 160, 36, [this] { startBc(); });
    btnLiveStop_ = button("Stop broadcast", 174, y, 150, 36, [] { A.ctl->stopBroadcast(); });
    y += 44;
    lblSm_ = label("", 8, y, 760, 24, true); y += 28;
    lblBc_ = label("", 8, y, 760, 112); y += 116;
    setContentHeight(y);
  }
  void activated() override { if (!filled_) { filled_ = true; auto b = ctl().broadcastSettings(); SetWindowTextW(edHost_, W(b.endpoint.host).c_str()); SetWindowTextW(edName_, W(b.endpoint.name.empty() ? "Studio ingest" : b.endpoint.name).c_str());
      SendMessageW(cbProto_, CB_SETCURSEL, (int)b.endpoint.protocol, 0); SetWindowTextW(edPort_, W(std::to_string(b.endpoint.port)).c_str()); if (!b.endpoint.path.empty()) SetWindowTextW(edPath_, W(b.endpoint.path).c_str());
      auto d = ctl().dynamics(); SendMessageW(chkBypass_, BM_SETCHECK, d.bypass, 0); SendMessageW(chkComp_, BM_SETCHECK, d.compressorOn, 0); SendMessageW(chkLim_, BM_SETCHECK, d.limiterOn, 0);
      float v[9] = {d.trimDb, d.thresholdDb, d.ratio, d.attackMs, d.releaseMs, d.kneeDb, d.makeupDb, d.ceilingDb, d.limiterReleaseMs}; for (int i = 0; i < 9; ++i) SetWindowTextW(dyn_[i], W(fmt("%g", (double)v[i])).c_str()); protoNote(); } }
  void refresh() override {
    auto st = ctl().status(); auto rs = ctl().recordingStatus(); auto bs = ctl().broadcastStatus(); auto su = ctl().setupState();
    bool rec = rs.state == RecState::Recording || rs.state == RecState::Preparing || rs.state == RecState::Stopping;
    setEnabled(btnRecStart_, !rec); setEnabled(btnRecStop_, rec);
    std::string t = fmt("State: %s | elapsed %s | folder: %s\r\n", recStateName(rs.state), fmtTime(st.recElapsedSec).c_str(), su.recordingFolder.empty() ? "(not set - choose one in Setup)" : su.recordingFolder.c_str());
    t += "Disk: " + (rs.disk.known ? rs.disk.text : std::string("free space unknown")) + (rs.disk.warning ? "  [LOW DISK WARNING]" : "") + "\r\n";
    t += fmt("Segments: %zu | written %llu MB | gaps: %llu (%llu frames) | audio blocks dropped %llu | video dropped %llu\r\n", rs.segments.size(), (unsigned long long)(rs.bytesWritten >> 20), (unsigned long long)rs.gapCount, (unsigned long long)rs.gapFrames, (unsigned long long)rs.audioBlocksDropped, (unsigned long long)rs.videoDropped);
    if (!rs.error.ok()) t += "ERROR: " + rs.error.detail + "\r\n"; for (auto& w : rs.warnings) t += "Warning: " + w + "\r\n"; if (!rs.lost.empty()) t += fmt("LOST INTERVALS: %zu (see the recording journal)\r\n", rs.lost.size());
    setTextIf(lblRec_, t);
    meterSet(meter_, st.meterRmsDb, st.meterPeakDb, st.inputNearFullScale, st.possibleClipping);
    setTextIf(lblProc_, fmt("Processed (recording/broadcast bus): near-full-scale blocks %llu, possible clipping blocks %llu, limiter active %llu blocks, max gain reduction %.1f dB.\r\nRaw input and processed output are separate: an upstream clip warns even when the limiter reduces the output.",
      (unsigned long long)rs.nearFullScaleBlocks, (unsigned long long)rs.possibleClippingBlocks, (unsigned long long)rs.limiterActiveBlocks, (double)rs.maxGainReductionDb));
    auto d = ctl().dynamics(); setTextIf(lblDyn_, d.bypass ? "Dynamics: BYPASSED (equally delayed dry path)" : "Dynamics: ACTIVE");
    // broadcast
    bool live = bs.state != BroadcastState::Off && bs.state != BroadcastState::Error;
    setEnabled(btnLiveStart_, !live); setEnabled(btnLiveStop_, live || bs.state == BroadcastState::Error);
    static const BroadcastState order[] = {BroadcastState::Off, BroadcastState::Preparing, BroadcastState::Connecting, BroadcastState::Live, BroadcastState::Reconnecting, BroadcastState::Stopping, BroadcastState::Error};
    std::string sm; for (auto s : order) { std::string n = broadcastStateName(s); sm += (s == bs.state ? "[ " + n + " ]" : n) + (s == BroadcastState::Stopping ? "  |  " : "  >  "); }
    setTextIf(lblSm_, "State machine: " + sm.substr(0, sm.size() - 5));
    std::string b = "Status: " + (bs.text.empty() ? std::string(broadcastStateName(bs.state)) : bs.text) + (bs.live ? "   *** LIVE ***" : "") + "\r\n";
    b += "Endpoint: " + (bs.endpoint.empty() ? std::string("(none)") : bs.endpoint) + " | " + (bs.encryption.empty() ? "" : bs.encryption) + "\r\n";
    if (bs.state == BroadcastState::Reconnecting) b += fmt("Reconnect attempt %d, next try in %.0f s, outage %.0f s\r\n", bs.attempt, std::max<int64_t>(0, bs.reconnectInMs) / 1000.0, bs.outageMs / 1000.0);
    b += fmt("Bitrate %.0f / %d kb/s | packets sent %llu, dropped %llu | queue %llu | reconnects %llu\r\n", bs.bitrateKbps, bs.targetKbps, (unsigned long long)bs.packetsSent, (unsigned long long)bs.packetsDropped, (unsigned long long)bs.queueDepth, (unsigned long long)bs.reconnects);
    if (!bs.error.ok()) b += "ERROR" + std::string(bs.errorTerminal ? " (terminal)" : "") + ": " + bs.error.detail + "\r\n";
    setTextIf(lblBc_, b);
    // summary
    PublicationInputs pi; pi.cameraInProgram = st.cameraPublished; pi.localRecordingActive = rs.state == RecState::Recording; pi.dynamicsActive = !d.bypass;
    std::string sum = publicationSummary(ctl().broadcastSettings(), pi); std::string crlf; for (char c : sum) { if (c == '\n') crlf += "\r\n"; else crlf += c; }
    if (getText(edSum_) != crlf) SetWindowTextW(edSum_, W(crlf).c_str());
  }
 private:
  void protoNote() {
    int i = comboSel(cbProto_); BroadcastProtocol p = (BroadcastProtocol)std::max(0, i); std::string issue = A.protocolIssue ? A.protocolIssue(p) : "";
    std::string t = issue.empty() ? std::string("Available.") : "UNAVAILABLE: " + issue;
    if (p != BroadcastProtocol::RTP && A.encoders) { auto av = A.encoders->availability(VideoCodec::H264); t += av.available ? std::string(" H.264 encoder: ") + av.name + (av.qualified ? "" : " (not qualified: verify on this machine)") : " H.264 unavailable: " + av.reason; }
    if (p == BroadcastProtocol::RTP) t += " RTP is unencrypted; MJPEG + L16.";
    setTextIf(lblProto_, t);
  }
  void startRec() {
    RecordingProfile p; int f = comboSel(cbFmt_); p.container = f == 1 ? RecContainer::Avi : f == 2 ? RecContainer::Mkv : RecContainer::Wav;
    if (p.container != RecContainer::Wav) { p.video.codec = VideoCodec::MJPEG; p.video.width = 800; p.video.height = 600; p.video.fpsNum = 25; }
    ctl().configureRecording(p); act(ActionId::StartRecording);
  }
  void applyDyn() {
    DynamicsConfig c; c.bypass = SendMessageW(chkBypass_, BM_GETCHECK, 0, 0) == BST_CHECKED; c.compressorOn = SendMessageW(chkComp_, BM_GETCHECK, 0, 0) == BST_CHECKED; c.limiterOn = SendMessageW(chkLim_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    float* f[9] = {&c.trimDb, &c.thresholdDb, &c.ratio, &c.attackMs, &c.releaseMs, &c.kneeDb, &c.makeupDb, &c.ceilingDb, &c.limiterReleaseMs};
    for (int i = 0; i < 9; ++i) { std::string s = getText(dyn_[i]); if (!s.empty()) *f[i] = (float)atof(s.c_str()); }
    ctl().setDynamics(c.clamped());
  }
  bool build(BroadcastSettings& b, std::string& err) {
    b = defaultBroadcastSettings(); int pre = comboSel(cbPre_); if (pre == 1) b = baseFallbackSettings(false); else if (pre == 2) b = baseFallbackSettings(true);
    b.endpoint.name = getText(edName_); b.endpoint.protocol = (BroadcastProtocol)std::max(0, comboSel(cbProto_)); b.endpoint.host = getText(edHost_);
    b.endpoint.port = (uint16_t)atoi(getText(edPort_).c_str()); b.endpoint.path = getText(edPath_);
    if (b.endpoint.protocol == BroadcastProtocol::RTMP || b.endpoint.protocol == BroadcastProtocol::RTMPS) b.endpoint.streamKeyRef = "streamkey";
    if (b.endpoint.protocol == BroadcastProtocol::RTP) { b.videoCodec = VideoCodec::MJPEG; b.endpoint.path.clear(); }
    int vk = atoi(getText(edVk_).c_str()), ak = atoi(getText(edAk_).c_str()); if (vk > 0) b.videoKbps = vk; if (ak > 0) b.audioKbps = ak;
    b.reconnect = SendMessageW(chkRe_, BM_GETCHECK, 0, 0) == BST_CHECKED; (void)err; return true;
  }
  void applySettings() { BroadcastSettings b; std::string e; build(b, e); Status s = ctl().configureBroadcast(b); postNotice(s.ok() ? "Broadcast settings applied." : "Broadcast settings: " + s.detail, !s.ok()); }
  void storeKey() {
    std::string k = getText(edKey_); if (k.empty()) { postNotice("Enter the stream key first.", true); return; }
    Status s = ctl().storeSecret("streamkey", k); SetWindowTextW(edKey_, L""); k.assign(k.size(), '\0');
    setTextIf(lblKey_, s.ok() ? "Key stored securely (hidden)." : "Key NOT stored: " + s.detail); postNotice(s.ok() ? "Stream key stored in the secret store." : "Key not stored: " + s.detail, !s.ok());
  }
  void startBc() {
    BroadcastSettings b; std::string e; build(b, e); Status s = ctl().configureBroadcast(b); if (!s.ok()) { postNotice("Cannot start: " + s.detail, true); return; }
    auto st = ctl().status(); PublicationInputs pi; pi.cameraInProgram = st.cameraPublished; pi.dynamicsActive = !ctl().dynamics().bypass; pi.localRecordingActive = ctl().recordingStatus().state == RecState::Recording;
    std::string sum = publicationSummary(b, pi);
    if (msgBox(A.mainWnd, sum + "\n\nStart broadcasting now?", "Start broadcast", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    UserStartToken tok; s = ctl().startGate().issue(ActionSource::UI, tok); if (!s.ok()) { postNotice("Start refused: " + s.detail, true); return; }
    s = ctl().startBroadcast(std::move(tok), pi); postNotice(s.ok() ? "Broadcast starting..." : "Broadcast not started: " + s.detail, !s.ok());
  }
  HWND cbFmt_ = nullptr, btnRecStart_ = nullptr, btnRecStop_ = nullptr, lblRec_ = nullptr, meter_ = nullptr, lblProc_ = nullptr, chkBypass_ = nullptr, chkComp_ = nullptr, chkLim_ = nullptr, dyn_[9] = {}, lblDyn_ = nullptr,
       edName_ = nullptr, cbProto_ = nullptr, lblProto_ = nullptr, edHost_ = nullptr, edPort_ = nullptr, edPath_ = nullptr, edKey_ = nullptr, lblKey_ = nullptr, cbPre_ = nullptr, edVk_ = nullptr, edAk_ = nullptr,
       chkRe_ = nullptr, edSum_ = nullptr, btnLiveStart_ = nullptr, btnLiveStop_ = nullptr, lblSm_ = nullptr, lblBc_ = nullptr;
  bool filled_ = false;
};
} // namespace
std::unique_ptr<Page> makeRecordPage() { return std::unique_ptr<Page>(new RecordPage()); }
}} // namespace ss::ui
