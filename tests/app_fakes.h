// Shared fakes + rig for test_app_*.cpp (header only; not a test target). Every platform port is a fake: no Windows, no sockets.
#pragma once
#include "test.h"
#include "core/app/controller.h"
#include "lighting/mock_sink.h"
#include "midi/mock_port.h"
#include "media/placeholder.h"
#include "broadcast/transport.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unistd.h>
#include <cstring>
#include <thread>

namespace apptest {
using namespace ss;
using namespace ss::app;

struct EventLog {
  std::mutex m; std::vector<std::string> v;
  void add(const std::string& s) { std::lock_guard<std::mutex> l(m); v.push_back(s); }
  std::vector<std::string> get() { std::lock_guard<std::mutex> l(m); return v; }
  int index(const std::string& s) { auto g = get(); for (size_t i = 0; i < g.size(); ++i) if (g[i] == s) return (int)i; return -1; }
  int count(const std::string& s) { int n = 0; for (auto& e : get()) if (e == s) ++n; return n; }
};

// ---- audio ---------------------------------------------------------------------------------------------------------
struct FakeAudio : IAudioCapture {
  EventLog* log = nullptr;
  std::vector<AudioDeviceInfo> devs{{"mic1", "Studio Mic", true, 2, 48000}, {"mic2", "Backup Mic", false, 2, 48000}};
  std::function<void(AudioBlockPtr)> blockCb; std::function<void(const AudioStatus&)> statusCb;
  AudioStatus st; std::string openedId; int opens = 0; std::vector<std::string> openLog;
  uint64_t nextSample = 0; uint64_t seq = 0; Ns t = 0;
  std::mutex m;
  std::vector<AudioDeviceInfo> enumerate() override { return devs; }
  Status open(const AudioOpenParams& p) override {
    if (log) log->add("audio.open:" + p.deviceId);
    ++opens; openLog.push_back(p.deviceId);
    bool known = false; std::string name; for (auto& d : devs) if (d.id == p.deviceId) { known = true; name = d.name; }
    AudioStatus s; s.deviceName = name; s.rate = 48000; s.channels = 2; s.generation = st.generation + 1;
    if (!known) { s.state = AudioState::Unavailable; s.detail = "device not found"; set(s); return Status::Fail(Err::DeviceLost, "device not found"); }
    s.state = AudioState::Running; openedId = p.deviceId; nextSample = 0; set(s); return Status::Ok();
  }
  void close() override { if (log) log->add("audio.close"); AudioStatus s = st; s.state = AudioState::Closed; set(s); }
  AudioStatus status() const override { return st; }
  void setBlockCallback(std::function<void(AudioBlockPtr)> cb) override { blockCb = std::move(cb); }
  void setStatusCallback(std::function<void(const AudioStatus&)> cb) override { statusCb = std::move(cb); }
  void set(const AudioStatus& s) { st = s; if (statusCb) statusCb(s); }
  void lose() { AudioStatus s = st; s.state = AudioState::Disconnected; s.detail = "device removed"; set(s); }
  // 120 BPM kick: 60 Hz burst (60 ms, decaying) at every 500 ms, plus a little hi-hat noise-free tone. amp 0 => digital silence.
  AudioBlockPtr makeBlock(int frames, double amp = 0.8, uint32_t flags = 0) {
    auto b = std::make_shared<AudioBlock>(); b->rate = 48000; b->channels = 2; b->firstSample = nextSample; b->captureNs = t; b->flags = flags | AF_TimestampValid;
    b->generation = st.generation; b->sequence = ++seq; b->samples.resize((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
      uint64_t n = nextSample + (uint64_t)i; double sec = (double)n / 48000.0; double ph = std::fmod(sec, 0.5);
      double env = ph < 0.06 ? std::exp(-ph * 45.0) : 0.0;
      float v = (float)(amp * env * std::sin(2.0 * 3.14159265358979 * 60.0 * sec));
      b->samples[(size_t)i * 2] = v; b->samples[(size_t)i * 2 + 1] = v;
    }
    nextSample += (uint64_t)frames; t += (Ns)frames * kNsPerSec / 48000;
    return b;
  }
  void feedMs(int ms, double amp = 0.8) { if (blockCb && st.state == AudioState::Running) blockCb(makeBlock(48 * ms, amp)); else { nextSample += 48u * ms; t += ms * kNsPerMs; } }
};

// ---- displays -------------------------------------------------------------------------------------------------------
struct FakeDisplays : IDisplayProvider {
  std::vector<DisplayInfo> list; std::function<void()> cb;
  std::vector<DisplayInfo> enumerate() override { return list; }
  void setChangeCallback(std::function<void()> c) override { cb = std::move(c); }
  void set(std::vector<DisplayInfo> v) { list = std::move(v); if (cb) cb(); }
  static DisplayInfo disp(const std::string& id, int idx, int x, int w, int h, bool primary = false, bool amb = false) {
    DisplayInfo d; d.stableId = id; d.name = "Monitor " + id; d.index = idx; d.x = x; d.y = 0; d.w = w; d.h = h; d.primary = primary; d.ambiguous = amb; return d;
  }
};

// ---- camera ---------------------------------------------------------------------------------------------------------
struct FakeCamera : ICamera {
  EventLog* log = nullptr;
  std::vector<CameraInfo> cams{{"cam1", "USB Cam"}, {"cam2", "Other Cam"}};
  CameraStatus st; std::function<void(const CameraStatus&)> cb; uint32_t color = 0x00FF00; bool producing = true; std::string openedId; int opens = 0;
  std::vector<CameraInfo> enumerate() override { return cams; }
  Status open(const std::string& id, CameraFormat want) override {
    ++opens; for (auto& c : cams) if (c.id == id) { openedId = id; st.state = CameraState::Live; st.negotiated = want; if (cb) cb(st); return Status::Ok(); }
    st.state = CameraState::Error; if (cb) cb(st); return Status::Fail(Err::DeviceLost, "no such camera");
  }
  void close() override { if (log) log->add("camera.close"); st.state = CameraState::Closed; if (cb) cb(st); }
  CameraStatus status() const override { return st; }
  bool latest(Frame& out) override {
    if (st.state != CameraState::Live || !producing) return false;
    out.resize(160, 120); for (auto& p : out.px) p = color; ++st.frames; return true;
  }
  void setStatusCallback(std::function<void(const CameraStatus&)> c) override { cb = std::move(c); }
  void lose() { st.state = CameraState::Lost; producing = false; if (cb) cb(st); }
};

struct FakeSecrets : ISecretStore {
  std::map<std::string, std::string> m; int puts = 0;
  Status put(const std::string& r, const std::string& s) override { m[r] = s; ++puts; return Status::Ok(); }
  Status get(const std::string& r, std::string& out) override { auto it = m.find(r); if (it == m.end()) return Status::Fail(Err::InvalidArgument, "missing"); out = it->second; return Status::Ok(); }
  Status remove(const std::string& r) override { m.erase(r); return Status::Ok(); }
};

// Real sink spy that also logs open/close ordering.
struct SpySink : RecordingSink {
  EventLog* log = nullptr;
  Status open(const std::string& nic, bool b) override { if (log) log->add("dmx.open"); return RecordingSink::open(nic, b); }
  void close() override { if (log) log->add("dmx.close"); RecordingSink::close(); }
};

struct TmpDir {
  std::filesystem::path p;
  TmpDir() {
    static int n = 0;
    p = std::filesystem::temp_directory_path() / ("ss_app_" + std::to_string((long)getpid()) + "_" + std::to_string(++n));
    std::filesystem::remove_all(p); std::filesystem::create_directories(p);
  }
  ~TmpDir() { std::error_code ec; std::filesystem::remove_all(p, ec); }
  std::string path(const std::string& rel = "") const { return rel.empty() ? p.string() : (p / rel).string(); }
};

inline show::ShowDocument testDoc(int nScenes = 4) {
  static const char* kinds[] = {"spectrum_bars", "plasma", "tunnel", "color_field", "radial_pulse", "grid_pulse", "particle_field", "ripple_rings"};
  show::ShowDocument d;
  d.uuid = "123e4567-e89b-12d3-a456-426614174000"; d.name = "App Test Show";
  for (int i = 0; i < nScenes; ++i) { show::Scene s; s.id = "s" + std::to_string(i); s.name = "Scene " + std::to_string(i); s.rendererType = kinds[i % 8]; d.scenes.push_back(s); }
  show::Output a; a.logicalId = "main"; show::Output b; b.logicalId = "alt"; show::Output c; c.logicalId = "ctl"; c.role = show::OutputRole::Controls;
  d.outputs = {a, b, c};
  d.autoCfg.minDwellMs = 1000; d.autoCfg.maxDwellMs = 1500; d.autoCfg.quantize = show::Quantize::Immediate; d.autoCfg.repeatHistory = 1;
  d.autoCfg.transition = {"cut", 0};
  return d;
}

// Everything a controller test needs, wired with fakes. Manual-pump by default (FakeClock, fully deterministic).
struct Rig {
  TmpDir dir; FakeClock clk; EventLog log;
  FakeAudio audio; FakeDisplays disp; FakeCamera cam; FakeSecrets secrets; MockMidiPort midi{&clk}; SpySink sink;
  MemFileSystem recFs; FakeDiskInfo disk; FakeCpuCounters cpu; FakeMemCounters mem;
  TransportFactory transports; FaultyTransport* lastTransport = nullptr;
  AppConfig cfg; AppDeps deps;
  std::unique_ptr<AppController> ctl;

  explicit Rig(bool manual = true, bool startNow = true, std::function<void(AppConfig&)> tweak = nullptr) {
    audio.log = &log; cam.log = &log; sink.log = &log;
    disp.list = {FakeDisplays::disp("A", 0, 0, 1024, 768, true), FakeDisplays::disp("B", 1, 1024, 320, 240), FakeDisplays::disp("C", 2, 1344, 320, 240)};
    cfg.dataDir = dir.path("data"); cfg.manualPump = manual; cfg.previewW = 64; cfg.previewH = 48; cfg.programW = 64; cfg.programH = 48;
    cfg.autoOpenSavedAudio = true; cfg.lighting.host = "192.168.1.50";
    if (tweak) tweak(cfg);
    disk.free = 100ull << 30;
    transports.setTestOverride([this] { auto t = std::unique_ptr<FaultyTransport>(new FaultyTransport()); lastTransport = t.get(); return std::unique_ptr<ITransport>(std::move(t)); });
    deps.clock = manual ? static_cast<IClock*>(&clk) : &systemClock();
    deps.audio = &audio; deps.displays = &disp; deps.camera = &cam; deps.midiIn = &midi; deps.midiOut = &midi; deps.lightingSink = &sink; deps.secrets = &secrets;
    deps.recFs = &recFs; deps.disk = &disk; deps.transports = &transports; deps.cpu = &cpu; deps.mem = &mem;
    midi.addPort({"in1", "Controller", true}); midi.addPort({"out1", "Synth", false});
    make();
    if (startNow) CHECK(ctl->start().ok());
  }
  void make() { ctl.reset(new AppController(cfg, deps)); }
  // one 10 ms tick: clock, audio, pump
  void tick(double amp = 0.8, bool feed = true) { clk.advance(10 * kNsPerMs); if (feed) audio.feedMs(10, amp); ctl->pump(); }
  void run(int ms, double amp = 0.8, bool feed = true) { for (int t = 0; t < ms; t += 10) tick(amp, feed); }
  ActionResult act(ActionId id, double v = 0, ActionSource src = ActionSource::UI, int target = -1) {
    Action a; a.id = id; a.value = v; a.source = src; a.target = target;
    auto f = ctl->submit(a); ctl->pump();
    return f.get();
  }
  void assignMain(const std::string& displayId) {
    for (auto& d : disp.list) if (d.stableId == displayId) { Status s = ctl->assignOutput("main", d); CHECK(s.ok()); if (!s.ok()) std::printf("  assign: %s\n", s.detail.c_str()); }
    ctl->refreshDisplays();
  }
  bool frame(const std::string& id, FramePtr& f) { return ctl->outputFrame(id, f); }
  static bool allZero(const Frame& f) { for (auto p : f.px) if (p) return false; return true; }
  static bool anyNonZero(const Frame& f) { return !allZero(f); }
};

inline int threadCount() {
  FILE* f = fopen("/proc/self/status", "r"); if (!f) return -1;
  char line[256]; int n = -1;
  while (fgets(line, sizeof line, f)) if (strncmp(line, "Threads:", 8) == 0) n = atoi(line + 8);
  fclose(f); return n;
}
template <class F> bool waitFor(F f, int ms = 3000) {
  auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < dl) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
  return f();
}
} // namespace apptest
