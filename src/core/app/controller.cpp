// AppController: lifecycle, control thread, action routing, ordered shutdown. See controller.h for the threading model.
#include "core/app/controller_impl.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ss { namespace app {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

bool isCriticalAction(ActionId id) {
  switch (id) {
    case ActionId::VisualBlackout: case ActionId::LightingBlackout: case ActionId::EmergencyBlackout: case ActionId::RestoreBlackout:
    case ActionId::DisarmLighting: case ActionId::StopRecording: case ActionId::StopBroadcast: case ActionId::DisableStrobing:
    case ActionId::PauseAutomation:
      return true;
    default: return false;
  }
}
const char* sourceLabel(const Estimate& e) {
  if (!e.valid) return "Unknown";
  return e.source == Estimate::Manual ? "Manual" : e.source == Estimate::Tap ? "Tap" : "Estimated";
}
std::string keyNameOf(const Estimate& k) {
  if (!k.valid) return "Unknown";
  static const char* n[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  int pc = ((int)k.value % 12 + 12) % 12;
  return std::string(n[pc]) + (k.minor ? " minor" : " major");
}

// ===================================================================================================================
// construction
// ===================================================================================================================
AppController::Impl::Impl(const AppConfig& c, const AppDeps& dd)
    : cfg(c), d(dd), clk(dd.clock ? dd.clock : &systemClock()),
      actQ(c.actionQueueCap ? c.actionQueueCap : 256), capQ(c.captureQueueNs), castQ(c.consumerAudioQueueNs),
      metrics(*clk), adapt(c.adaptation) {
  pfiles = d.files ? d.files : &persist::defaultFileOps();
  dfiles = d.diagFiles ? d.diagFiles : &stdFileOps();
  if (d.encoders) encoders = d.encoders; else { ownedEncoders = EncoderRegistry::withBuiltins(); encoders = &ownedEncoders; }
  lcfg = cfg.lighting; lib = ProfileLibrary::builtin();
  gate.reset(new UserActionGate(clk));
  mediaRoot = cfg.mediaRoot;
  { RotationConfig rc; rc.dir = joinPath(cfg.dataDir, "logs"); rc.baseName = "stagesignal"; rc.ext = ".log";
    logFile.reset(new RotatingFile(*dfiles, rc));
    logger.reset(new Logger(*clk, *logFile)); logger->setRedactor(&redactor);
    RotationConfig jc = rc; jc.baseName = "journal"; jc.ext = ".jsonl";
    journalFile.reset(new RotatingFile(*dfiles, jc));
    journal.reset(new EventJournal(*clk)); journal->attachFile(journalFile.get()); journal->setRedactor(&redactor); }
  media::MediaConfig mc; mc.tier = media::Tier::Base; mc.clock = clk;
  mediaMgr.reset(new media::MediaManager(mc));
  if (d.makeVideoDecoder) {
    media::DecoderEntry e; e.id = "injected"; e.containers = {"mp4", "mov", "mkv"}; e.codecs = {"h264", "h265", "vp8", "vp9", "av1"}; e.factory = d.makeVideoDecoder;
    decoders.registerDecoder(e); decoders.loadManifestAvailability("{\"decoders\":[{\"id\":\"injected\",\"enabled\":true}]}");
    mediaMgr->setDecoderRegistry(decoders);
  }
  adapt.setLocked(cfg.qualityLocked);
  adapt.setEventCallback([this](const AdaptationEvent& e) {
    { std::lock_guard<std::mutex> l(diagMu); adaptHistory.push_back({e.t, std::string(adaptEventKindName(e.kind)) + " level=" + std::to_string(e.level) + " step=" + adaptStepName(e.step) + " " + e.reason}); if (adaptHistory.size() > 200) adaptHistory.erase(adaptHistory.begin()); }
    jrec(JournalKind::Adaptation, adaptEventKindName(e.kind), std::string(adaptStepName(e.step)) + ": " + e.reason, e.level);
    logInfo("adaptation", std::string(adaptEventKindName(e.kind)) + " level=" + std::to_string(e.level) + " step=" + adaptStepName(e.step) + " " + e.reason);
  });
  quality = adapt.state();
  sceneFpsX100 = (int)(quality.sceneFps * 100); previewFpsX100 = (int)(quality.previewFps * 100); scaleX1000 = (int)(quality.renderScale * 1000);
  engine.reset(new show::ShowEngine());
  engine->setJournalHook([this](const std::string& k, const std::string& j) { onShowJournal(k, j); });
  mrouter.reset(new MidiRouter(table, graph, *clk));
  mrouter->setActionSink([this](const Action& a) { submitAction(a, AckCallback()); });
  mrouter->setAlarmCallback([this](const MidiAlarm& al) {
    const char* kind = al.kind == MidiAlarmKind::PortLost ? "port_lost" : al.kind == MidiAlarmKind::PortReconnected ? "port_reconnected" : al.kind == MidiAlarmKind::InputOverflow ? "input_overflow" : al.kind == MidiAlarmKind::RateCeiling ? "rate_ceiling" : "echo_loop";
    jrec(JournalKind::Device, std::string("midi:") + kind, al.detail);
    if (al.kind == MidiAlarmKind::PortLost) setWarning("midi_in", "MIDI input lost: mappings for that device are unavailable until you reconnect it.", false);
    if (al.kind == MidiAlarmKind::EchoLoop) setWarning("midi_echo", "MIDI feedback loop detected: the route was suspended. Re-enable it explicitly after fixing the cabling.", true);
  });
  mrouter->setLostForward([this](int, bool input) {
    if (input) midiInLost = true; else { midiOutLost = true; if (mout) mout->notePortLost(); }
    wake();
  });
  adoptDocument(defaultDocument(), false, false);
  publishView(clk->nowNs());
}

AppController::Impl::~Impl() {
  // all threads are joined (or detached after a timeout, in which case they hold a shared_ptr and we never get here)
}

show::ShowDocument AppController::Impl::defaultDocument() const {
  show::ShowDocument doc;
  doc.uuid = "00000000-0000-4000-8000-000000000001"; doc.name = "Untitled show";
  const auto& pack = show::rendererPack();
  for (size_t i = 0; i < pack.size() && i < 8; ++i) { show::Scene s; s.id = "scene" + std::to_string(i + 1); s.name = pack[i].id; s.rendererType = pack[i].id; doc.scenes.push_back(s); }
  show::Output a; a.logicalId = "main"; a.role = show::OutputRole::Audience;
  show::Output p; p.logicalId = "program"; p.role = show::OutputRole::Program;
  show::Output c; c.logicalId = "controls"; c.role = show::OutputRole::Controls; c.overlay.text = false;
  doc.outputs = {a, p, c};
  return doc;
}

// ===================================================================================================================
// diagnostics wrappers
// ===================================================================================================================
void AppController::Impl::logInfo(const std::string& c, const std::string& m) { std::lock_guard<std::mutex> l(diagMu); logger->info(c, m); }
void AppController::Impl::logWarn(const std::string& c, const std::string& m) { std::lock_guard<std::mutex> l(diagMu); logger->warn(c, m); }
void AppController::Impl::jrec(JournalKind k, const std::string& subject, const std::string& detail, double a, double b) {
  std::lock_guard<std::mutex> l(diagMu); journal->record(k, subject, detail, a, b);
}
void AppController::Impl::setWarning(const std::string& id, const std::string& text, bool critical) {
  std::lock_guard<std::mutex> l(warnMu); Warning w; w.id = id; w.text = text; w.critical = critical; warns[id] = w;
}
void AppController::Impl::clearWarning(const std::string& id) { std::lock_guard<std::mutex> l(warnMu); warns.erase(id); }

void AppController::Impl::onShowJournal(const std::string& kind, const std::string& js) {
  JournalKind k = JournalKind::Note;
  if (kind == "scene_change") k = JournalKind::Scene;
  else if (kind.compare(0, 4, "cue_") == 0) k = JournalKind::Cue;
  else if (kind == "mode_change" || kind == "override" || kind == "pause" || kind == "resume") k = JournalKind::Mode;
  else if (kind == "tempo_key") return;                 // journaled at <=1 Hz from the effective estimates in housekeeping()
  std::string subject = kind;
  if (k == JournalKind::Scene) { json j = json::parse(js, nullptr, false); if (j.is_object() && j.contains("scene") && j["scene"].is_string()) subject = j["scene"].get<std::string>(); }
  jrec(k, subject, k == JournalKind::Scene ? std::string("scene_change") : js);
}

// ===================================================================================================================
// persistence helpers (settings)
// ===================================================================================================================
void AppController::Impl::settingsPut(const std::string& k, const std::string& v) { std::lock_guard<std::mutex> l(setMu); if (v.empty()) settings.devices.erase(k); else settings.devices[k] = v; }
std::string AppController::Impl::settingsGet(const std::string& k) const {
  std::lock_guard<std::mutex> l(const_cast<std::mutex&>(setMu)); auto it = settings.devices.find(k); return it == settings.devices.end() ? std::string() : it->second;
}
Status AppController::Impl::saveSettingsNow() {
  persist::LocalSettings s;
  { std::lock_guard<std::mutex> l(setMu); s = settings; }
  s.outputDisplays = router.assignments();
  { std::lock_guard<std::mutex> l(setMu); settings.outputDisplays = s.outputDisplays; }
  std::string cd = router.controlsDisplay(); if (!cd.empty()) s.devices["controlsDisplay"] = cd;
  pfiles->createDirs(cfg.dataDir);
  return persist::saveSettings(*pfiles, persist::joinPath(cfg.dataDir, "settings.json"), s);
}

// ===================================================================================================================
// lifecycle
// ===================================================================================================================
Status AppController::Impl::start(const std::shared_ptr<Impl>& self) {
  std::lock_guard<std::mutex> lk(lifeMu);
  if (started.load()) return Status::Ok();
  if (shutDone.load()) return Status::Fail(Err::Cancelled, "controller was shut down");
  pfiles->createDirs(cfg.dataDir);
  { persist::LocalSettings s; std::string sp = persist::joinPath(cfg.dataDir, "settings.json");
    if (pfiles->exists(sp)) {
      Status ls = persist::loadSettings(*pfiles, sp, s);
      if (ls.ok()) { std::lock_guard<std::mutex> l(setMu); settings = s; }
      else { logWarn("settings", "local settings unreadable, using defaults: " + ls.detail); setWarning("settings", "Saved local settings could not be read; defaults are in use. Re-run setup.", false); }
    } }
  { std::lock_guard<std::mutex> l(setMu);
    router.setAssignments(settings.outputDisplays);
    auto cd = settings.devices.find("controlsDisplay"); if (cd != settings.devices.end()) router.setControlsDisplay(cd->second);
    auto lp = settings.devices.find("lightingProtocol");
    if (lp != settings.devices.end()) {
      lcfg.protocol = lp->second == "sacn" ? LightProtocol::Sacn : lp->second == "usb" ? LightProtocol::UsbDmx : LightProtocol::ArtNet;
      auto h = settings.devices.find("lightingHost"); if (h != settings.devices.end()) lcfg.host = h->second;
      auto n = settings.devices.find("lightingNic"); if (n != settings.devices.end()) lcfg.nicAddr = n->second;
      lcfg.sacnMulticast = settings.devices.count("lightingMulticast") && settings.devices["lightingMulticast"] == "1";
      lcfg.allowBroadcast = settings.devices.count("lightingBroadcast") && settings.devices["lightingBroadcast"] == "1";
    }
  }
  // crash recovery: detect BEFORE beginSession; recovery itself is offered, never applied silently
  crash.reset(new persist::CrashRecovery(*pfiles, cfg.dataDir));
  recov = crash->detect();
  crash->beginSession(clk->wallUnixMs());
  if (recov.crashed) {
    logWarn("recovery", "previous session ended unexpectedly");
    setWarning("recovery", "The previous session did not close cleanly. Recovery is offered in Setup; DMX, recording and broadcast stay off.", false);
  }
  { std::string bj; persist::IFileOps& fo = *pfiles; std::vector<uint8_t> bytes;
    if (persist::readFileAll(fo, persist::joinPath(cfg.dataDir, "bench_history.json"), 1u << 20, bytes).ok()) benchHistory.fromJson(std::string(bytes.begin(), bytes.end()));
    if (persist::readFileAll(fo, persist::joinPath(cfg.dataDir, "broadcast.json"), 1u << 20, bytes).ok()) {
      BroadcastSettings bs; if (importSettingsJson(std::string(bytes.begin(), bytes.end()), bs).ok()) { std::lock_guard<std::mutex> l(bcMu); bcSettings = bs; } } }
  // ports
  if (d.audio) {
    d.audio->setBlockCallback([this](AudioBlockPtr b) { onAudioBlock(std::move(b)); });
    d.audio->setStatusCallback([this](const AudioStatus& s) { onAudioStatus(s); });
  }
  if (d.displays) d.displays->setChangeCallback([this] { displaysDirty = true; wake(); });
  if (d.camera) d.camera->setStatusCallback([this](const CameraStatus& s) { camStatBox.put(s); wake(); });
  if (d.midiOut && d.midiOut != d.midiIn) d.midiOut->setLostCallback([this](bool) { midiOutLost = true; if (mout) mout->notePortLost(); wake(); });
  refreshDisplaysNow();
  buildLighting(*docPtr);
  if (cfg.autoOpenSavedAudio && d.audio) {          // reopen the SAVED id only; failure leaves the status at Unavailable (REQ-A02)
    std::string id = settingsGet("audioInput");
    if (!id.empty()) {
      AudioOpenParams p; p.deviceId = id; std::string r = settingsGet("audioRoute"); p.channelRoute = r.empty() ? 0 : atoi(r.c_str());
      Status s = openAudio(p, false);
      if (!s.ok()) { logWarn("audio", "saved audio device could not be opened: " + s.detail); setWarning("audio_open", "The saved audio input could not be opened (" + s.detail + "). No other device was selected.", true); }
    }
  }
  restoreSavedMidi();
  metrics.reset();
  Ns now = clk->nowNs();
  showStartNs = now; nextRender = now; nextPreview = now; nextAdapt = now + cfg.adaptationPeriodNs; nextSec = now + kNsPerSec; nextLight = now;
  started = true; selfHold = self;
  logInfo("app", "started");
  if (!cfg.manualPump) {
    exAnalysis = false; exRender = false; exControl = false; exLight = false; exIo = false;
    std::shared_ptr<Impl> s = self;
    thIo = std::thread([s] { s->ioLoop(); s->exIo = true; });
    thControl = std::thread([s] { s->controlLoop(); s->exControl = true; });
    thAnalysis = std::thread([s] { s->analysisLoop(); s->exAnalysis = true; });
    thRender = std::thread([s] { s->renderLoop(); s->exRender = true; });
    thLight = std::thread([s] { s->lightingLoop(); s->exLight = true; });
  }
  return Status::Ok();
}

void AppController::Impl::sleepUntil(Ns t, const std::atomic<bool>& stopFlag) {
  for (;;) {
    if (stopFlag.load()) return;
    Ns rem = t - clk->nowNs();
    if (rem <= 0) return;
    Ns slice = std::min<Ns>(rem, 2 * kNsPerMs);
    std::this_thread::sleep_for(std::chrono::nanoseconds(slice));
  }
}

bool AppController::Impl::joinWithin(std::thread& t, std::atomic<bool>& exited, int timeoutMs) {
  if (!t.joinable()) return true;
  auto dl = Clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!exited.load() && Clock::now() < dl) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (exited.load()) { t.join(); return true; }
  t.detach();            // keeps running holding a shared_ptr to Impl; reported as timed out
  return false;
}

namespace {
template <class F> void runStage(ShutdownReport& rep, const char* name, int timeoutMs, F&& fn) {
  ShutdownStage st; st.name = name;
  auto t0 = Clock::now();
  bool ok = true; bool to = false; std::string detail;
  fn(ok, to, detail);
  st.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  if (st.elapsedMs > timeoutMs) { to = true; ok = false; detail += (detail.empty() ? "" : "; ") + std::string("stage exceeded its timeout"); }
  st.ok = ok; st.timedOut = to; st.detail = detail;
  if (!ok) rep.clean = false;
  rep.stages.push_back(st);
}
}

ShutdownReport AppController::Impl::shutdown() {
  {
    std::lock_guard<std::mutex> lk(lifeMu);
    if (shutDone.exchange(true)) return lastShutdown;
  }
  ShutdownReport rep;
  const int T = cfg.shutdownStageTimeoutMs;
  stopping = true; cancelBench = true;
  // 1. disarm outward actions
  runStage(rep, "disarm_outputs", T, [&](bool&, bool&, std::string&) {
    doDisarmLighting("shutdown");
    if (mout) mout->setClockEnabled(false);
    jrec(JournalKind::Device, "shutdown", "outputs disarmed");
  });
  // 2. cancel broadcasters
  runStage(rep, "broadcast", T, [&](bool& ok, bool& to, std::string& dt) {
    doStopBroadcast();
    stopCast = true;
    if (!joinWithin(thCast, exCast, T)) { ok = false; to = true; dt = "cast thread did not exit"; }
  });
  // 3. finalize writers
  runStage(rep, "recording", T, [&](bool& ok, bool&, std::string& dt) {
    auto r = rec.get();
    if (r) { Status s = r->stop(); if (!s.ok()) { ok = false; dt = s.detail; } }
  });
  // 4. quiesce MIDI and lighting
  runStage(rep, "quiesce_midi_lighting", T, [&](bool& ok, bool& to, std::string& dt) {
    if (mout) mout->stop();
    if (mrouter) { mrouter->setActionSink(MidiRouter::ActionSink()); }
    if (d.midiIn) { d.midiIn->setLostCallback(nullptr); d.midiIn->closeInput(); }
    if (d.midiOut) { d.midiOut->setLostCallback(nullptr); d.midiOut->closeOutput(); }
    stopLight = true;
    if (!joinWithin(thLight, exLight, T)) { ok = false; to = true; dt = "lighting thread did not exit"; }
    std::lock_guard<std::mutex> l(lightMu); live.reset(); dry.reset(); liveArmed = false;
  });
  // 5. stop capture and camera
  runStage(rep, "stop_capture", T, [&](bool&, bool&, std::string&) {
    if (d.audio) { d.audio->close(); d.audio->setBlockCallback(nullptr); d.audio->setStatusCallback(nullptr); }
    if (d.camera) { d.camera->close(); d.camera->setStatusCallback(nullptr); }
    if (d.displays) d.displays->setChangeCallback(nullptr);
    { std::lock_guard<std::mutex> l(camMu); camOpen = false; camPub = CameraPublication(); }
  });
  // 6. drain bounded tasks
  runStage(rep, "drain_tasks", T, [&](bool& ok, bool& to, std::string& dt) {
    stopAnalysis = true; anCv.notify_all();
    if (!joinWithin(thAnalysis, exAnalysis, T)) { ok = false; to = true; dt = "analysis thread did not exit"; }
    stopControl = true; wake();
    if (!joinWithin(thControl, exControl, T)) { ok = false; to = true; dt += "; control thread did not exit"; }
    Task t; while (critQ.pop(t)) { if (t.cancel) t.cancel("controller is shutting down"); }
    while (actQ.pop(t)) { if (t.cancel) t.cancel("controller is shutting down"); }
    stopIo = true; { std::lock_guard<std::mutex> l(ioMu); } ioCv.notify_all();
    if (!joinWithin(thIo, exIo, T)) { ok = false; to = true; dt += "; io thread did not exit"; }
    while (ioStep()) {}
    if (thBench.joinable() && !joinWithin(thBench, exBench, T)) { ok = false; to = true; dt += "; benchmark thread did not exit"; }
  });
  // 7. destroy worker resources
  runStage(rep, "destroy_workers", T, [&](bool&, bool&, std::string&) {
    { std::lock_guard<std::mutex> l(mediaMu); handles.clear(); }
    mediaMgr.reset();
    analyzer.reset(); dynamics.reset();
    rec.set(nullptr); bc.set(nullptr);
    capQ.clear(); castQ.clear();
  });
  // 8. destroy renderer
  runStage(rep, "renderer", T, [&](bool& ok, bool& to, std::string& dt) {
    stopRender = true;
    if (!joinWithin(thRender, exRender, T)) { ok = false; to = true; dt = "render thread did not exit"; return; }
    slots.clear(); previewR.reset(); previewPool.clear();
    std::lock_guard<std::mutex> l(frameMu); for (auto& kv : frameBoxes) kv.second->box.put(FramePtr());
  });
  // 9. close UI-facing state: final flush, settings, clean-session marker
  runStage(rep, "ui_close", T, [&](bool& ok, bool&, std::string& dt) {
    Status s = saveSettingsNow(); if (!s.ok()) { ok = false; dt = "settings: " + s.detail; }
    if (crash && rep.clean) crash->endSession();
    logInfo("app", rep.clean ? "shutdown complete" : "shutdown complete with problems");
    { std::lock_guard<std::mutex> l(diagMu); journal->flush(); logger->flush(); }
  });
  lastShutdown = rep;
  selfHold.reset();
  return rep;
}

// ===================================================================================================================
// control thread
// ===================================================================================================================
void AppController::Impl::controlLoop() {
  uint64_t seen = 0;
  while (!stopControl.load()) {
    {
      std::unique_lock<std::mutex> l(wakeMu);
      wakeCv.wait_for(l, std::chrono::milliseconds((int)(1000.0 / cfg.controlHz)), [&] { return stopControl.load() || wakeSeq.load() != seen; });
      seen = wakeSeq.load();
    }
    if (stopControl.load()) break;
    controlStep(clk->nowNs());
  }
}

void AppController::Impl::submitAction(const Action& a, AckCallback ack) {
  auto reject = [&](const std::string& why) { if (ack) { ActionResult r; r.accepted = false; r.reason = why; ack(r); } };
  if (stopping.load() || stopControl.load()) { reject("controller is shutting down"); return; }
  Task t; t.enq = clk->nowNs(); t.ack = true;
  t.run = [this, a, ack](Ns now) { ActionResult r = routeAction(a, now); if (ack) ack(r); };
  t.cancel = [ack](const std::string& why) { if (ack) { ActionResult r; r.reason = why; ack(r); } };
  bool ok = isCriticalAction(a.id) ? critQ.push(t) : actQ.push(t);
  if (!ok) { ++actRejectedFull; reject("action queue full"); return; }
  wake();
}

bool AppController::Impl::pushTask(std::function<void(Ns)> run, std::function<void(const std::string&)> cancel) {
  if (stopping.load() || stopControl.load()) return false;
  Task t; t.enq = clk->nowNs(); t.run = std::move(run); t.cancel = std::move(cancel);
  if (!actQ.push(t)) { ++actRejectedFull; return false; }
  wake();
  return true;
}

std::future<Status> AppController::Impl::postStatus(std::function<Status(Ns)> fn) {
  auto pr = std::make_shared<std::promise<Status>>();
  std::future<Status> fut = pr->get_future();
  if (stopping.load() || stopControl.load()) { pr->set_value(Status::Fail(Err::Cancelled, "controller is shutting down")); return fut; }
  Task t; t.enq = clk->nowNs();
  t.run = [pr, fn](Ns now) { pr->set_value(fn(now)); };
  t.cancel = [pr](const std::string& why) { pr->set_value(Status::Fail(Err::Cancelled, why)); };
  if (!actQ.push(t)) { ++actRejectedFull; pr->set_value(Status::Fail(Err::ResourceExhausted, "action queue full")); return fut; }
  wake();
  return fut;
}

void AppController::Impl::publishView(Ns now) {
  auto v = std::make_shared<ShowView>();
  { std::lock_guard<std::mutex> l(docMu); v->doc = docPtr; }
  v->snap = engine->snapshot(); v->features = engine->effectiveFeatures(); v->docGeneration = docGen; v->publishedNs = now;
  effBpm = v->snap.tempo.valid ? v->snap.tempo.value : 0.0;
  lastSnap = v->snap; lastEff = v->features;
  router.setBlackout(v->snap.visualBlackedOut);
  viewBox.put(std::move(v));
}

void AppController::Impl::refreshDisplaysNow() {
  if (!d.displays) return;
  std::vector<DisplayInfo> cur = d.displays->enumerate();
  std::vector<OutputRouteView> before = router.views();
  router.updateDisplays(cur);
  std::vector<OutputRouteView> after = router.views();
  for (const auto& a : after) {
    RouteState prev = RouteState::Unassigned; bool had = false;
    for (const auto& b : before) if (b.logicalId == a.logicalId) { prev = b.state; had = true; }
    if (had && prev != a.state) {
      jrec(JournalKind::Device, "display:" + a.logicalId, std::string(routeStateName(prev)) + " -> " + routeStateName(a.state));
      logInfo("display", "output '" + a.logicalId + "' " + routeStateName(prev) + " -> " + routeStateName(a.state));
    }
  }
  saveSettingsNow();
}

void AppController::Impl::controlStep(Ns now) {
  if (displaysDirty.exchange(false)) refreshDisplaysNow();
  if (audioAvailDirty.exchange(false)) {
    bool av = audioAvail.load();
    engine->setAudioAvailable(av);
    AudioStatus st; if (audioStatBox.get(st)) {
      jrec(JournalKind::Device, "audio", std::string(av ? "running: " : "unavailable: ") + st.deviceName + " " + st.detail);
      if (av) { clearWarning("audio_lost"); clearWarning("audio_open"); }
      else setWarning("audio_lost", "Audio input " + std::string(st.state == AudioState::Denied ? "access denied" : st.state == AudioState::Disconnected ? "disconnected" : "unavailable") + ": " + st.deviceName + ". StageSignal does not switch to another device automatically; reselect or reconnect it.", true);
    }
  }
  if (midiInLost.exchange(false)) { jrec(JournalKind::Device, "midi:input", "lost"); setWarning("midi_in", "MIDI input lost: mappings for that device are unavailable until you reconnect it.", false); }
  if (midiOutLost.exchange(false)) { jrec(JournalKind::Device, "midi:output", "lost"); setWarning("midi_out", "MIDI output lost. Reconnect it explicitly; queued messages were discarded.", false); }
  if (mrouter) mrouter->pump();
  Task t; int n = 0;
  auto runTask = [&](Task& tk) { if (tk.run) tk.run(now); if (tk.ack) metrics.latency(LatencyKind::UiAck, std::max<Ns>(0, clk->nowNs() - tk.enq)); ++n; };
  while (n < 64 && critQ.pop(t)) runTask(t);
  while (n < 192 && actQ.pop(t)) runTask(t);
  const Ns period = (Ns)(1e9 / cfg.controlHz);
  if (lastUpdate < 0 || now - lastUpdate >= period - period / 10 || n > 0) {
    FeatureSnapshot f; featBox.get(f);
    engine->update(now, f); lastUpdate = now;
  }
  // lighting: engine-side arm request and the live engine must agree. The app NEVER re-arms by itself.
  show::ShowSnapshot snap = engine->snapshot();
  if (liveArmed.load() && (liveFaulted.load() || !snap.lightingArmRequested)) {
    doDisarmLighting(liveFaulted.load() ? "lighting fault" : "arm request cleared");
    if (liveFaulted.exchange(false)) setWarning("lighting_fault", "Lighting transport fault: DMX output is disarmed. Fix the link, then arm again explicitly.", true);
  }
  if (snap.lightingArmRequested && !liveArmed.load()) { Action da; da.id = ActionId::DisarmLighting; da.source = ActionSource::System; engine->submit(da, now); }
  publishView(now);
  syncLightingLatches(lastSnap);
  if (now >= nextAdapt) { adaptationTick(now); nextAdapt = now + cfg.adaptationPeriodNs; if (nextAdapt < now) nextAdapt = now + cfg.adaptationPeriodNs; }
  if (now >= nextSec) { housekeeping(now); nextSec = now + kNsPerSec; }
}

ActionResult AppController::Impl::routeAction(const Action& a, Ns now) {
  ActionResult r;
  auto rej = [&](const std::string& why) { r.accepted = false; r.reason = why; return r; };
  switch (a.id) {
    case ActionId::StartRecording: {
      Status s = doStartRecording(a.source);
      if (!s.ok()) return rej(s.detail);
      r.accepted = true; break;
    }
    case ActionId::StopRecording: {
      auto rs = rec.get();
      if (!rs || (rs->state() != RecState::Recording && rs->state() != RecState::Preparing)) { r.accepted = true; r.reason = "not recording"; break; }
      auto keep = rs;
      if (!postIo([keep, this] { Status s = keep->stop(); jrec(JournalKind::Record, "stop", s.ok() ? "completed" : s.detail); })) return rej("io queue full");
      r.accepted = true; break;
    }
    case ActionId::AddMarker: {
      static std::atomic<int> mk{0};
      std::string label = "Marker " + std::to_string(++mk);
      auto rs = rec.get();
      if (rs && rs->state() == RecState::Recording) rs->addMarker(label);
      jrec(JournalKind::Marker, label);
      r.accepted = true; break;
    }
    case ActionId::StartBroadcast: {
      UserStartToken tok; Status g = gate->issue(a.source, tok);
      if (!g.ok()) return rej(g.detail);
      Status s = doStartBroadcast(std::move(tok), makePublication());
      if (!s.ok()) return rej(s.detail);
      r.accepted = true; break;
    }
    case ActionId::StopBroadcast: doStopBroadcast(); r.accepted = true; break;
    case ActionId::ArmLighting: {
      if (a.source != ActionSource::UI && a.source != ActionSource::Keyboard && a.source != ActionSource::Midi && a.source != ActionSource::Remote)
        return rej("lighting can be armed only by an explicit operator action");
      r = engine->submit(a, now);
      if (r.accepted) {
        Status s = doArmLighting();
        if (!s.ok()) { Action da; da.id = ActionId::DisarmLighting; da.source = ActionSource::System; engine->submit(da, now); return rej(s.detail); }
      }
      break;
    }
    case ActionId::DisarmLighting: r = engine->submit(a, now); doDisarmLighting("operator"); r.accepted = true; break;
    default: r = engine->submit(a, now); break;
  }
  if (r.accepted) {
    show::ShowSnapshot s = engine->snapshot();
    router.setBlackout(s.visualBlackedOut);            // final composition goes black by the next published frame
    syncLightingLatches(s);
    r.revision = s.revision;
    switch (a.id) {
      case ActionId::SetBpm: case ActionId::ReleaseBpm: case ActionId::TapTempo: case ActionId::SetKey: case ActionId::ReleaseKey: case ActionId::DisableStrobing:
        docDirty = true; if (dirtySince < 0) dirtySince = now; break;
      default: break;
    }
    if (actionMayEmitMidi(a)) midiFeedback(a, a.value);
  } else if (r.reason.empty()) r.reason = "rejected";
  return r;
}

Status AppController::Impl::adoptDocument(const show::ShowDocument& doc, bool recovery, bool markDirty) {
  if (recovery) {
    show::EngineOptions o; o.startIdle = true;
    bool v = lastSnap.visualBlackoutLatch, l = lastSnap.lightingBlackoutLatch, e = lastSnap.emergencyBlackoutLatch;
    engine.reset(new show::ShowEngine(o));
    engine->setJournalHook([this](const std::string& k, const std::string& j) { onShowJournal(k, j); });
    Ns now = clk->nowNs();
    if (v) { Action a; a.id = ActionId::VisualBlackout; a.source = ActionSource::System; engine->submit(a, now); }
    if (l) { Action a; a.id = ActionId::LightingBlackout; a.source = ActionSource::System; engine->submit(a, now); }
    if (e) { Action a; a.id = ActionId::EmergencyBlackout; a.source = ActionSource::System; engine->submit(a, now); }
  }
  Status s = engine->replaceDocument(doc);
  if (!s.ok()) return s;                                   // old show stays active (REQ-S14)
  doDisarmLighting("show replaced");
  auto np = std::make_shared<show::ShowDocument>(engine->document());
  { std::lock_guard<std::mutex> l(docMu); docPtr = np; }
  ++docGen;
  router.setOutputs(np->outputs);
  loadMidiFromDoc(*np);
  buildLighting(*np);
  registerMedia(*np);
  lastLookRef.clear();
  if (markDirty) { docDirty = true; if (dirtySince < 0) dirtySince = clk->nowNs(); } else { docDirty = false; dirtySince = -1; }
  if (recovery) { recovered = true; setWarning("recovered", "Recovered show: the output is idle and DMX, recording and broadcast are off until you start them.", false); }
  jrec(JournalKind::Note, "show_loaded", np->name);
  logInfo("show", "document active: " + np->name);
  publishView(clk->nowNs());
  return Status::Ok();
}

void AppController::Impl::adaptationTick(Ns now) {
  PressureSample s; s.now = now;
  if (d.cpu) { CpuSample c = d.cpu->sample(); cachedCpu.c = c; s.cpuAvailable = c.available; s.processCpuPct = c.processPct; s.systemCpuPct = c.systemPct; }
  s.deadlinesEvaluated = deadlinesEval.load(); s.deadlinesMissed = deadlinesMissed.load();
  double enc = castQ.fraction(); double recQ = 0;
  if (auto r = rec.get()) { RecordingStatus rs = r->status(); recQ = std::min(1.0, (double)rs.queuedAudioFrames / (double)std::max<uint32_t>(1, r->profile().audioRate * 2)); }
  if (auto b = bc.get()) { BroadcastStatus bs = b->status(); enc = std::max(enc, std::min(1.0, (double)bs.queueDepth / 512.0)); }
  enc = std::max(enc, recQ);
  s.encoderQueueFraction = enc;
  s.maxQueueFraction = std::max({enc, capQ.fraction(), (double)actQ.size() / (double)std::max<size_t>(1, actQ.capacity())});
  QualityState q = adapt.update(s);
  quality = q;
  qLevel = q.level; sceneFpsX100 = (int)(q.sceneFps * 100.0 + 0.5); previewFpsX100 = (int)(q.previewFps * 100.0 + 0.5); scaleX1000 = (int)(q.renderScale * 1000.0 + 0.5);
  trailsReduced = q.trailsParticlesReduced;
  if (q.level > 0) setWarning("adaptation", std::string("Quality reduced to protect the show (step: ") + adaptStepName(q.lastStep) + "). Output scene rate " + std::to_string((int)q.sceneFps) + " FPS.", false);
  else clearWarning("adaptation");
  if (adapt.overloadActive() && adapt.locked()) setWarning("overload_locked", "System overloaded while quality is locked: quality was NOT reduced and added work is refused.", true);
  else clearWarning("overload_locked");
}

void AppController::Impl::housekeeping(Ns now) {
  // MIDI port health poll (fallback to the lost callback)
  if (mrouter) mrouter->checkPorts();
  // tempo/key journal (<=1 Hz, unknown stays unknown)
  { std::lock_guard<std::mutex> l(diagMu); journal->recordTempoKey(lastEff.tempo, lastEff.key); }
  // bounded-queue telemetry
  metrics.queueSample("capture_audio", (size_t)(capQ.fraction() * 1000), 1000);
  metrics.queueSample("cast_audio", (size_t)(castQ.fraction() * 1000), 1000);
  metrics.queueSample("actions", actQ.size(), actQ.capacity());
  metrics.sampleCounters(d.cpu ? &cachedCpu : nullptr, d.mem);
  // camera / route / recording / broadcast / media warnings
  CameraStatus cs; if (camStatBox.get(cs)) {
    if (cs.state == CameraState::Lost || cs.state == CameraState::Error) setWarning("camera", "Camera " + std::string(cs.state == CameraState::Lost ? "lost" : "error") + ": the camera layer is hidden. No other camera was selected.", false);
    else clearWarning("camera");
  }
  for (const auto& v : router.views()) {
    std::string id = "route:" + v.logicalId;
    if (v.state == RouteState::Disconnected && v.role != show::OutputRole::Controls) setWarning(id, "Output '" + v.logicalId + "' display disconnected; controls are unaffected. Reconnect the same screen to restore.", false);
    else if (v.state == RouteState::NeedsReassign) setWarning(id, "Output '" + v.logicalId + "': identical monitors detected; pick the screen again to resume.", false);
    else if (v.state == RouteState::Blocked) setWarning(id, "Output '" + v.logicalId + "': " + v.note, false);
    else clearWarning(id);
  }
  if (auto r = rec.get()) {
    RecordingStatus rs = r->status();
    if (rs.state == RecState::Failed || rs.state == RecState::Recoverable) setWarning("recording", "Recording stopped: " + rs.error.detail, true);
    else if (!rs.warnings.empty()) setWarning("recording", rs.warnings.back(), false);
    else clearWarning("recording");
  }
  if (auto b = bc.get()) {
    BroadcastStatus bs = b->status();
    if (bs.state == BroadcastState::Error) setWarning("broadcast", "Broadcast error: " + bs.error.detail, true);
    else if (bs.state == BroadcastState::Reconnecting) setWarning("broadcast", "Broadcast reconnecting (attempt " + std::to_string(bs.attempt) + "); local recording continues.", false);
    else clearWarning("broadcast");
  }
  { std::vector<std::string> miss; { std::lock_guard<std::mutex> l(mediaMu); for (auto& kv : handles) if (kv.second.h && kv.second.h->state() == media::MediaHandle::State::Failed) miss.push_back(kv.first); }
    if (!miss.empty()) setWarning("media", std::to_string(miss.size()) + " media asset(s) missing or invalid: a placeholder is shown.", false); else clearWarning("media"); }
  { std::string mj = table.toJson(); if (lastMidiJson.empty()) lastMidiJson = mj; else if (mj != lastMidiJson) { lastMidiJson = mj; docDirty = true; if (dirtySince < 0) dirtySince = now; } }
  // autosave (show JSON only, via the io thread)
  if (docDirty.load() && crash && dirtySince >= 0 && now - dirtySince >= cfg.autosaveInterval) {
    show::ShowDocument doc = engine->documentWithState(); syncMidiToDoc(doc);
    std::string path = crash->autosavePath(); dirtySince = now;
    postIo([this, doc, path] { persist::ShowIO io(*pfiles); Status s = io.save(path, doc, 1); if (!s.ok()) logWarn("autosave", "autosave failed: " + s.detail); });
  }
  { std::lock_guard<std::mutex> l(diagMu); journal->flush(); }
}

// ===================================================================================================================
// MIDI helpers
// ===================================================================================================================
void AppController::Impl::loadMidiFromDoc(const show::ShowDocument& doc) {
  if (doc.midiMappings.empty()) { table.clear(); lastMidiJson = table.toJson(); return; }
  json env; env["schema"] = "stagesignal.midi.mappings"; env["version"] = 1; env["mappings"] = json::array();
  for (const auto& m : doc.midiMappings) { json j = json::parse(m, nullptr, false); if (!j.is_discarded()) env["mappings"].push_back(j); }
  Status s = table.fromJson(env.dump());
  if (!s.ok()) { table.clear(); setWarning("midi_map", "MIDI mappings in this show could not be loaded: " + s.detail, false); logWarn("midi", "mapping load failed: " + s.detail); }
  else clearWarning("midi_map");
  lastMidiJson = table.toJson();
}
void AppController::Impl::syncMidiToDoc(show::ShowDocument& doc) const {
  json j = json::parse(table.toJson(), nullptr, false);
  doc.midiMappings.clear();
  if (j.is_object() && j.contains("mappings") && j["mappings"].is_array()) for (const auto& m : j["mappings"]) doc.midiMappings.push_back(m.dump());
}
void AppController::Impl::midiFeedback(const Action& a, double value) {
  if (!mout || midiOutLost.load()) return;
  std::vector<MidiFeedbackRule> rules; { std::lock_guard<std::mutex> l(fbMu); rules = fbRules; }
  for (const auto& r : rules) {
    if (r.action != a.id) continue;
    if (!graph.isActive("out:feedback")) continue;
    int v = value != 0 ? r.valueOn : r.valueOff;
    if (r.type == MidiMsgType::Note) mout->noteOn(r.channel, r.number, v ? v : 1, 0, MidiOrigin::Operator);
    else if (r.type == MidiMsgType::CC) mout->cc(r.channel, r.number, v, 0, MidiOrigin::Operator);
    else mout->programChange(r.channel, r.number, 0, MidiOrigin::Operator);
  }
}

// ===================================================================================================================
// pump (manual mode)
// ===================================================================================================================
void AppController::pump() {
  Impl& I = *p_;
  if (!I.started.load() || I.shutDone.load()) return;
  Ns now = I.clk->nowNs();
  I.analysisStep(64);
  I.controlStep(now);
  if (I.mout) I.mout->step();
  I.ioStep();
  I.renderStep(I.clk->nowNs());
  I.lightingStep(I.clk->nowNs());
  I.castStep(I.clk->nowNs());
}

// ===================================================================================================================
// public API: lifecycle, actions, show
// ===================================================================================================================
AppController::AppController(const AppConfig& cfg, const AppDeps& deps) : p_(std::make_shared<Impl>(cfg, deps)) {}
AppController::~AppController() { if (p_) p_->shutdown(); }
Status AppController::start() { return p_->start(p_); }
ShutdownReport AppController::shutdown() { return p_->shutdown(); }
bool AppController::running() const { return p_->started.load() && !p_->shutDone.load(); }

std::future<ActionResult> AppController::submit(const Action& a) {
  auto pr = std::make_shared<std::promise<ActionResult>>();
  std::future<ActionResult> f = pr->get_future();
  p_->submitAction(a, [pr](const ActionResult& r) { pr->set_value(r); });
  return f;
}
void AppController::submit(const Action& a, AckCallback ack) { p_->submitAction(a, std::move(ack)); }
UserActionGate& AppController::startGate() { return *p_->gate; }
Status AppController::startBroadcast(UserStartToken&& token, const PublicationInputs& pub) {
  PublicationInputs pi = pub;
  if (pi.cameraLabel.empty() && p_->programCameraPublished()) { pi.cameraInProgram = true; std::lock_guard<std::mutex> l(p_->camMu); pi.cameraLabel = p_->camInfo.name; }
  return p_->doStartBroadcast(std::move(token), pi);
}
void AppController::stopBroadcast() { p_->doStopBroadcast(); }

std::future<Status> AppController::applyDocument(const show::ShowDocument& doc) {
  Impl* I = p_.get();
  return I->postStatus([I, doc](Ns) { return I->adoptDocument(doc, false, true); });
}
std::shared_ptr<const show::ShowDocument> AppController::document() const { std::lock_guard<std::mutex> l(p_->docMu); return p_->docPtr; }
show::ShowSnapshot AppController::showSnapshot() const { ShowViewPtr v; if (p_->viewBox.get(v) && v) return v->snap; return show::ShowSnapshot(); }
bool AppController::showDirty() const { return p_->docDirty.load(); }

std::future<Status> AppController::setDraftText(const std::string& s) { Impl* I = p_.get(); return I->postStatus([I, s](Ns) { return I->engine->setDraft(s); }); }
std::future<Status> AppController::previewDraftText() { Impl* I = p_.get(); return I->postStatus([I](Ns) { return I->engine->previewDraft(); }); }
std::future<Status> AppController::publishDraftText(int target) {
  Impl* I = p_.get();
  return I->postStatus([I, target](Ns) {
    Status s = I->engine->publishDraft(target);
    if (s.ok()) { ShowViewPtr v; I->jrec(JournalKind::TextPublished, I->engine->snapshot().previewText.empty() ? "draft" : "message"); }
    return s;
  });
}

AppStatus AppController::status() const { return p_->buildStatus(); }
std::vector<OutputRouteView> AppController::outputRoutes() const { return p_->routeViews(); }
bool AppController::outputFrame(const std::string& id, FramePtr& out) const {
  std::shared_ptr<FrameBox> b;
  { std::lock_guard<std::mutex> l(p_->frameMu); auto it = p_->frameBoxes.find(id); if (it == p_->frameBoxes.end()) return false; b = it->second; }
  FramePtr f; if (!b->box.get(f) || !f) return false; out = f; return true;
}
bool AppController::previewFrame(FramePtr& out) const { FramePtr f; if (!p_->previewBox.get(f) || !f) return false; out = f; return true; }
bool AppController::cameraPreviewFrame(FramePtr& out) const { FramePtr f; if (!p_->camPreviewBox.get(f) || !f) return false; out = f; return true; }
MetricsSnapshot AppController::metrics() { return p_->metrics.snapshot(); }
FeatureSnapshot AppController::features() const { FeatureSnapshot f; p_->featBox.get(f); return f; }
MappingTable& AppController::midiMappings() { return p_->table; }
MidiRouteGraph& AppController::midiRoutes() { return p_->graph; }

}} // namespace ss::app
