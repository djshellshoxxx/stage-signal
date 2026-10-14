// AppController: persistence (open/save/autosave/recovery/portable), setup wizard model, status snapshot, benchmark, diagnostics.
#include "core/app/controller_impl.h"
#include "core/app/app_bench.h"
#include "nlohmann/json.hpp"
#include <algorithm>

namespace ss { namespace app {

using json = nlohmann::json;

// ===================================================================================================================
// io worker
// ===================================================================================================================
bool AppController::Impl::postIo(std::function<void()> f) {
  if (!ioQ.push(std::move(f))) return false;
  { std::lock_guard<std::mutex> l(ioMu); }
  ioCv.notify_one();
  return true;
}
void AppController::Impl::ioLoop() {
  for (;;) {
    std::function<void()> f;
    if (ioQ.pop(f)) { f(); continue; }
    if (stopIo.load()) break;
    std::unique_lock<std::mutex> l(ioMu);
    ioCv.wait_for(l, std::chrono::milliseconds(50), [&] { return stopIo.load() || ioQ.size() > 0; });
  }
}
size_t AppController::Impl::ioStep() {
  size_t n = 0; std::function<void()> f;
  while (ioQ.pop(f)) { f(); ++n; }
  return n;
}

namespace {
using StatusPromise = std::shared_ptr<std::promise<Status>>;
void failP(const StatusPromise& pr, Err e, const std::string& why) { pr->set_value(Status::Fail(e, why)); }
}

// ===================================================================================================================
// show lifecycle
// ===================================================================================================================
std::future<Status> AppController::openShow(const std::string& path) {
  Impl* I = p_.get();
  auto pr = std::make_shared<std::promise<Status>>(); std::future<Status> fut = pr->get_future();
  bool ok = I->postIo([I, pr, path] {
    persist::ShowIO io(*I->pfiles); show::ShowDocument doc; persist::LoadInfo li;
    Status s = io.loadWithFallback(path, doc, &li);
    if (!s.ok()) { I->logWarn("show", "open failed (running show unchanged): " + s.detail); pr->set_value(s); return; }   // REQ-S14
    std::string dir = persist::parentDir(path);
    bool queued = I->pushTask([I, pr, doc, path, dir](Ns) {
      { std::lock_guard<std::mutex> l(I->pathMu); I->mediaRoot = dir; }
      Status a = I->adoptDocument(doc, false, false);
      if (a.ok()) { std::lock_guard<std::mutex> l(I->pathMu); I->currentPath = path; }
      pr->set_value(a);
    }, [pr](const std::string& why) { failP(pr, Err::Cancelled, why); });
    if (!queued) failP(pr, Err::ResourceExhausted, "action queue full or shutting down");
  });
  if (!ok) failP(pr, Err::ResourceExhausted, "io queue full");
  return fut;
}

std::future<Status> AppController::saveShow(const std::string& path) {
  Impl* I = p_.get();
  auto pr = std::make_shared<std::promise<Status>>(); std::future<Status> fut = pr->get_future();
  bool ok = I->pushTask([I, pr, path](Ns) {
    std::string target = path;
    if (target.empty()) { std::lock_guard<std::mutex> l(I->pathMu); target = I->currentPath; }
    if (target.empty()) { failP(pr, Err::InvalidArgument, "choose a file name to save the show"); return; }
    show::ShowDocument doc = I->engine->documentWithState(); I->syncMidiToDoc(doc);
    bool q = I->postIo([I, pr, target, doc] {
      persist::ShowIO io(*I->pfiles);
      Status s = io.save(target, doc);
      if (s.ok()) { { std::lock_guard<std::mutex> l(I->pathMu); I->currentPath = target; } I->docDirty = false; I->dirtySince = -1; I->logInfo("show", "saved"); }
      else I->logWarn("show", "save failed: " + s.detail);
      pr->set_value(s);
    });
    if (!q) failP(pr, Err::ResourceExhausted, "io queue full");
  }, [pr](const std::string& why) { failP(pr, Err::Cancelled, why); });
  if (!ok) failP(pr, Err::ResourceExhausted, "action queue full or shutting down");
  return fut;
}

std::future<Status> AppController::exportPortable(const std::string& zipPath, const persist::ExportOptions& opt) {
  Impl* I = p_.get();
  auto pr = std::make_shared<std::promise<Status>>(); std::future<Status> fut = pr->get_future();
  bool ok = I->pushTask([I, pr, zipPath, opt](Ns) {
    show::ShowDocument doc = I->engine->documentWithState(); I->syncMidiToDoc(doc);
    persist::ExportOptions o = opt;
    if (o.mediaRoot.empty()) { std::lock_guard<std::mutex> l(I->pathMu); o.mediaRoot = I->mediaRoot; }
    bool q = I->postIo([I, pr, zipPath, doc, o] {
      persist::ExportReport rep;
      Status s = persist::exportBundle(*I->pfiles, doc, o, zipPath, &rep);
      I->logInfo("show", s.ok() ? "portable export written" : "portable export failed: " + s.detail);
      pr->set_value(s);
    });
    if (!q) failP(pr, Err::ResourceExhausted, "io queue full");
  }, [pr](const std::string& why) { failP(pr, Err::Cancelled, why); });
  if (!ok) failP(pr, Err::ResourceExhausted, "action queue full or shutting down");
  return fut;
}

std::future<Status> AppController::importPortable(const std::string& archivePath, const std::string& destDir) {
  Impl* I = p_.get();
  auto pr = std::make_shared<std::promise<Status>>(); std::future<Status> fut = pr->get_future();
  bool ok = I->postIo([I, pr, archivePath, destDir] {
    persist::ImportResult res = persist::importBundle(*I->pfiles, archivePath, destDir);
    if (!res.status.ok()) { I->logWarn("show", "import rejected (running show unchanged): " + res.status.detail); pr->set_value(res.status); return; }
    bool q = I->pushTask([I, pr, res](Ns) {
      { std::lock_guard<std::mutex> l(I->pathMu); I->mediaRoot = res.mediaRoot; }
      Status a = I->adoptDocument(res.doc, false, true);
      if (a.ok()) { std::lock_guard<std::mutex> l(I->pathMu); I->currentPath = res.showFile; }
      if (a.ok() && !res.mediaComplete) I->setWarning("import_media", "Imported show is missing some media; placeholders are shown until the files are added.", false);
      pr->set_value(a);
    }, [pr](const std::string& why) { failP(pr, Err::Cancelled, why); });
    if (!q) failP(pr, Err::ResourceExhausted, "action queue full or shutting down");
  });
  if (!ok) failP(pr, Err::ResourceExhausted, "io queue full");
  return fut;
}

persist::RecoveryInfo AppController::recoveryInfo() const { std::lock_guard<std::mutex> l(p_->setMu); return p_->recov; }

std::future<Status> AppController::restoreRecovery(const std::string& desiredPath) {
  Impl* I = p_.get();
  auto pr = std::make_shared<std::promise<Status>>(); std::future<Status> fut = pr->get_future();
  bool ok = I->postIo([I, pr, desiredPath] {
    if (!I->crash) { failP(pr, Err::InvalidArgument, "recovery is not available before start()"); return; }
    persist::RestoredShow rs; Status s = I->crash->restoreCopy(desiredPath, rs);
    if (!s.ok()) { pr->set_value(s); return; }
    bool q = I->pushTask([I, pr, rs](Ns) {
      Status a = I->adoptDocument(rs.doc, true, true);            // idle engine; lighting/broadcast/recording are never re-armed
      if (a.ok()) { { std::lock_guard<std::mutex> l(I->pathMu); I->currentPath = rs.savedPath; } std::lock_guard<std::mutex> l(I->setMu); I->recov.crashed = false; }
      pr->set_value(a);
    }, [pr](const std::string& why) { failP(pr, Err::Cancelled, why); });
    if (!q) failP(pr, Err::ResourceExhausted, "action queue full or shutting down");
  });
  if (!ok) failP(pr, Err::ResourceExhausted, "io queue full");
  return fut;
}
Status AppController::discardRecovery() {
  if (!p_->crash) return Status::Fail(Err::InvalidArgument, "not started");
  Status s = p_->crash->discard();
  { std::lock_guard<std::mutex> l(p_->setMu); p_->recov = persist::RecoveryInfo(); }
  p_->clearWarning("recovery");
  return s;
}

// ===================================================================================================================
// status snapshot
// ===================================================================================================================
std::vector<OutputRouteView> AppController::Impl::routeViews() const {
  Impl* self = const_cast<Impl*>(this);
  std::vector<OutputRouteView> v = router.views();
  ShowViewPtr sv; self->viewBox.get(sv);
  for (auto& r : v) {
    if (sv) for (const auto& os : sv->snap.outputs) if (os.logicalId == r.logicalId) r.independentScene = os.independentScene;
    std::lock_guard<std::mutex> l(self->frameMu);
    auto it = self->frameBoxes.find(r.logicalId);
    if (it != self->frameBoxes.end()) { r.framesPublished = it->second->published.load(); r.framesDropped = it->second->dropped.load(); }
  }
  return v;
}

AppStatus AppController::Impl::buildStatus() const {
  Impl* self = const_cast<Impl*>(this);
  AppStatus s; s.seq = ++self->statusSeq;
  AudioStatus as; bool hasAs = audioStatBox.get(as);
  s.audioState = hasAs ? as.state : AudioState::Closed;
  s.audioDevice = hasAs && !as.deviceName.empty() ? as.deviceName : (self->audioName);
  if (hasAs) s.audioDetail = as.detail;
  FeatureSnapshot f; featBox.get(f);
  if (s.audioState == AudioState::Running) {
    for (int i = 0; i < 2; ++i) { s.meterRmsDb[i] = f.rmsDb[i]; s.meterPeakDb[i] = f.peakDb[i]; }
    s.inputNearFullScale = f.inputNearFullScale; s.possibleClipping = f.possibleClipping;
  }
  s.captureOverruns = capOverruns.load();
  ShowViewPtr v; self->viewBox.get(v);
  if (v) {
    const show::ShowSnapshot& n = v->snap;
    s.mode = n.mode; s.paused = n.paused; s.manualOverride = n.manualOverride; s.audioLossPolicyEngaged = n.audioLost; s.idleActive = n.idleActive;
    s.bpmValid = n.tempo.valid; s.bpm = n.tempo.valid ? n.tempo.value : 0; s.bpmConfidence = n.tempo.confidence; s.bpmLabel = sourceLabel(n.tempo);
    s.keyValid = n.key.valid; s.keyName = keyNameOf(n.key); s.keyLabel = sourceLabel(n.key);
    s.visualBlackout = n.visualBlackedOut; s.lightingBlackout = n.lightingBlackedOut; s.emergencyBlackout = n.emergencyBlackoutLatch; s.strobeDisabled = n.strobeDisabled;
    s.showName = v->doc->name; s.showRevision = n.revision; s.sceneIndex = n.sceneIndex;
    if (n.sceneIndex >= 0 && n.sceneIndex < (int)v->doc->scenes.size()) s.sceneId = v->doc->scenes[(size_t)n.sceneIndex].id;
  }
  s.showDirty = docDirty.load(); s.recovered = recovered;
  { std::lock_guard<std::mutex> l(self->camMu); s.cameraName = camInfo.name; s.cameraPublished = camPub.published && camOpen;
    CameraStatus cs; if (camOpen && camStatBox.get(cs)) s.cameraState = cs.state; else if (camOpen) s.cameraState = CameraState::Live; else s.cameraState = CameraState::Closed; }
  s.dmxArmed = liveArmed.load(); s.dmxText = s.dmxArmed ? "Armed" : "Off";
  if (auto r = rec.get()) { RecordingStatus rs = r->status(); s.recState = rs.state; s.recElapsedSec = nsToSec(rs.timelineNs); }
  if (auto b = bc.get()) { BroadcastStatus bs = b->status(); s.liveState = bs.state; s.liveText = bs.live ? "LIVE" : broadcastStateName(bs.state); }
  s.outputs = routeViews();
  s.adaptationLevel = qLevel.load(); s.qualityLocked = adapt.locked();
  { QualityState q = quality; s.adaptationStep = q.level > 0 ? adaptStepName(q.lastStep) : "none"; }
  s.sceneFps = sceneFpsX100.load() / 100.0; s.previewFps = previewFpsX100.load() / 100.0; s.renderScale = scaleX1000.load() / 1000.0;
  s.midiInputAlive = d.midiIn && midiInIdx >= 0 && d.midiIn->inputAlive(); s.midiOutputAlive = d.midiOut && midiOutOpen.load() && d.midiOut->outputAlive();
  s.midiMappings = table.size(); s.midiMappingsUnavailable = table.unavailableMappings().size();
  { std::lock_guard<std::mutex> l(self->mediaMu); for (auto& kv : handles) if (kv.second.h && kv.second.h->state() == media::MediaHandle::State::Failed) ++s.mediaMissing; }
  s.actionQueueDepth = actQ.size(); s.captureQueueDepth = capQ.blocks(); s.castAudioQueueDepth = castQ.blocks();
  s.actionsRejectedFull = actRejectedFull.load(); s.captureDrops = capQ.drops(); s.castAudioDrops = castQ.drops();
  s.framesRendered = framesRendered.load(); s.deadlinesMissed = deadlinesMissed.load();
  { std::lock_guard<std::mutex> l(self->warnMu); for (auto& kv : warns) s.warnings.push_back(kv.second); }
  std::stable_sort(s.warnings.begin(), s.warnings.end(), [](const Warning& a, const Warning& b) { return a.critical && !b.critical; });
  return s;
}

// ===================================================================================================================
// setup wizard (UJ-01) -> LocalSettings
// ===================================================================================================================
SetupState AppController::Impl::buildSetupState() const {
  SetupState s;
  std::vector<OutputRouteView> views = router.views();
  bool displaysDone = !router.controlsDisplay().empty();
  for (const auto& v : views) {
    SetupState::DisplayRole r; r.logicalId = v.logicalId; r.role = v.role; r.displayId = v.displayId; r.state = v.state; s.displayRoles.push_back(r);
    if (v.role == show::OutputRole::Audience && v.state != RouteState::Active) displaysDone = false;
  }
  s.controlsDisplayId = router.controlsDisplay();
  s.audioDeviceId = settingsGet("audioInput"); s.audioDeviceName = settingsGet("audioInputName");
  { std::string r = settingsGet("audioRoute"); s.audioChannelRoute = r.empty() ? 0 : atoi(r.c_str()); }
  s.gainAcknowledged = settingsGet("setup.gainAck") == "1";
  s.cameraId = settingsGet("camera"); s.cameraPreviewed = settingsGet("setup.cameraPreviewed") == "1";
  s.midiInId = settingsGet("midiIn"); s.midiOutId = settingsGet("midiOut"); s.midiMappings = table.size();
  { std::string p = settingsGet("lightingProtocol"); s.lighting.protocol = p == "sacn" ? LightProtocol::Sacn : p == "usb" ? LightProtocol::UsbDmx : LightProtocol::ArtNet;
    s.lighting.host = settingsGet("lightingHost"); s.lighting.nicAddr = settingsGet("lightingNic"); s.lighting.sacnMulticast = settingsGet("lightingMulticast") == "1"; s.lighting.allowBroadcast = settingsGet("lightingBroadcast") == "1";
    s.lightingConfigured = settingsGet("setup.lighting") == "1"; }
  { std::lock_guard<std::mutex> l(const_cast<std::mutex&>(setMu)); s.recordingFolder = settings.recordingFolder; }
  s.benchmarkProfileHash = settingsGet("setup.benchmarkProfile"); s.benchmarkDone = !s.benchmarkProfileHash.empty(); s.benchmarkQualifying = settingsGet("setup.benchmarkQualifying") == "1";
  auto step = [&](SetupStepId id, const char* title, bool done, bool opt, const std::string& sum) { SetupStep t; t.id = id; t.title = title; t.complete = done; t.optional = opt; t.summary = sum; s.steps.push_back(t); };
  step(SetupStepId::Displays, "Displays and roles", displaysDone, false, displaysDone ? "controls and audience outputs assigned" : "assign the controls display and audience outputs (use Identify)");
  step(SetupStepId::Audio, "Audio input", !s.audioDeviceId.empty(), false, s.audioDeviceName.empty() ? "choose an input device" : s.audioDeviceName);
  step(SetupStepId::Gain, "Input gain", s.gainAcknowledged, false, "set hardware gain on the interface; peaks below -6 dBFS");
  step(SetupStepId::Camera, "Camera preview", s.cameraPreviewed, true, s.cameraId.empty() ? "optional" : "preview only, not published");
  step(SetupStepId::Midi, "MIDI mapping", s.midiMappings > 0, true, std::to_string(s.midiMappings) + " mapping(s)");
  step(SetupStepId::Lighting, "Lighting interface", s.lightingConfigured, true, "DMX stays off until armed explicitly");
  step(SetupStepId::Recording, "Recording folder", !s.recordingFolder.empty(), true, s.recordingFolder);
  step(SetupStepId::Benchmark, "Benchmark", s.benchmarkDone, false, s.benchmarkDone ? (s.benchmarkQualifying ? "qualified profile saved" : "scaled/unqualified result saved") : "run BEN-01 on this machine");
  bool all = true; for (const auto& t : s.steps) if (!t.optional && !t.complete) all = false;
  step(SetupStepId::Review, "Review", all, false, all ? "ready" : "complete the required steps");
  return s;
}
SetupState AppController::setupState() const { return p_->buildSetupState(); }
Status AppController::saveSetup() { return p_->saveSettingsNow(); }
persist::LocalSettings AppController::localSettings() const {
  persist::LocalSettings s; { std::lock_guard<std::mutex> l(p_->setMu); s = p_->settings; }
  s.outputDisplays = p_->router.assignments();
  return s;
}

// ===================================================================================================================
// benchmark
// ===================================================================================================================
BenchmarkOutcome AppController::Impl::runBenchmarkNow(const BenchmarkRequest& req) {
  BenchmarkOutcome out;
  AppBenchInputs in; in.clock = req.paceRealTime ? clk : req.fakeClock; in.fake = req.paceRealTime ? nullptr : req.fakeClock;
  in.sys = d.sys; in.cpu = d.cpu; in.mem = d.mem; in.encoders = encoders; in.text = nullptr; in.cancel = &cancelBench;
  in.width = req.width; in.height = req.height; in.scenarios = req.scenarios; in.durationScale = req.durationScale;
  jrec(JournalKind::Note, "benchmark_start", "mock sinks only");
  out.result = runAppBenchmark(in);
  out.profile = out.result.toProfile();
  std::string js = out.result.toJson();
  { std::lock_guard<std::mutex> l(benchMu); lastBenchJson = js; }
  pfiles->createDirs(persist::joinPath(cfg.dataDir, "bench"));
  out.jsonPath = persist::joinPath(persist::joinPath(cfg.dataDir, "bench"), "bench-" + std::to_string(out.result.wallMs) + ".json");
  out.status = persist::writeFileAll(*pfiles, out.jsonPath, js.data(), js.size());
  benchHistory.add(out.profile);
  { std::string h = benchHistory.toJson(); persist::writeFileAll(*pfiles, persist::joinPath(cfg.dataDir, "bench_history.json"), h.data(), h.size()); }
  settingsPut("setup.benchmarkProfile", out.profile.profileHash); settingsPut("setup.benchmarkQualifying", out.result.qualifying ? "1" : "");
  settingsPut("setup.hwKey", out.profile.hardwareKey); settingsPut("setup.appKey", out.profile.appKey);
  saveSettingsNow();
  jrec(JournalKind::Note, "benchmark_done", out.result.qualifying ? "qualifying" : "non-qualifying");
  return out;
}

std::future<BenchmarkOutcome> AppController::runBenchmark(const BenchmarkRequest& req) {
  Impl* I = p_.get();
  auto pr = std::make_shared<std::promise<BenchmarkOutcome>>(); std::future<BenchmarkOutcome> fut = pr->get_future();
  auto refuse = [&](Err e, const std::string& why) { BenchmarkOutcome o; o.status = Status::Fail(e, why); pr->set_value(o); return std::move(fut); };
  if (!req.paceRealTime && !req.fakeClock) return refuse(Err::InvalidArgument, "a deterministic benchmark needs a FakeClock");
  auto r = I->rec.get();
  if ((r && (r->state() == RecState::Recording || r->state() == RecState::Preparing)) || I->liveArmed.load() || I->castActive.load())
    return refuse(Err::PermissionDenied, "benchmark is refused while recording, broadcasting or DMX are active (it never touches live outputs)");
  if (I->benchRunning.exchange(true)) return refuse(Err::InvalidArgument, "a benchmark is already running");
  auto work = [I, pr, req] { BenchmarkOutcome o = I->runBenchmarkNow(req); I->benchRunning = false; pr->set_value(o); };
  if (I->cfg.manualPump) {
    if (!I->postIo(work)) { I->benchRunning = false; return refuse(Err::ResourceExhausted, "io queue full"); }
  } else {
    if (I->thBench.joinable()) { if (I->exBench.load()) I->thBench.join(); else { I->benchRunning = false; return refuse(Err::InvalidArgument, "previous benchmark thread still running"); } }
    I->exBench = false; I->cancelBench = false;
    std::shared_ptr<Impl> self = I->selfHold;
    if (!self) { I->benchRunning = false; return refuse(Err::Cancelled, "controller is not running"); }
    I->thBench = std::thread([self, work] { work(); self->exBench = true; });
  }
  return fut;
}

// ===================================================================================================================
// adaptation / admission / diagnostics
// ===================================================================================================================
void AppController::setQualityLocked(bool locked) { p_->adapt.setLocked(locked); p_->cfg.qualityLocked = locked; }

CapabilityStatus AppController::capability(const std::vector<FeatureDef>& requested) {
  Impl& I = *p_;
  AdmissionContext ctx;
  std::string hw = I.settingsGet("setup.hwKey"), ap = I.settingsGet("setup.appKey");
  ctx.hardwareKey = hw; ctx.appKey = ap; ctx.nowWallMs = I.clk->wallUnixMs();
  ctx.profile = hw.empty() ? nullptr : I.benchHistory.findMatching(hw, ap, ctx.nowWallMs, ctx.maxProfileAgeMs);
  ctx.encoderAvailable = I.encoders->availability(VideoCodec::MJPEG).available;
  ctx.overloadActive = I.adapt.overloadActive(); ctx.qualityLocked = I.adapt.locked(); ctx.quality = I.quality;
  if (I.d.mem) { MemSample m = I.d.mem->sample(); if (m.workingSetBytes.available) ctx.memUsedBytes = m.workingSetBytes.value; }
  if (!I.d.cpu) ctx.unavailableCounters.push_back("cpu");
  if (!I.d.mem) ctx.unavailableCounters.push_back("memory");
  ctx.unavailableCounters.push_back("gpu.time");
  return AdmissionController().evaluate(requested, ctx);
}

uint64_t AppController::addMarker(const std::string& label) {
  auto r = p_->rec.get();
  if (r && r->state() == RecState::Recording) r->addMarker(label);
  uint64_t seq;
  { std::lock_guard<std::mutex> l(p_->diagMu); seq = p_->journal->marker(label); }
  return seq;
}
std::vector<JournalEvent> AppController::journalSnapshot() const { std::lock_guard<std::mutex> l(p_->diagMu); return p_->journal->snapshot(); }

static void fillBundle(AppController::Impl& I, SupportBundle& sb) {
  sb.setRedactor(&I.redactor);
  sb.addLogSource(I.logFile.get());
  sb.setJournal(I.journal.get(), I.journalFile.get());
  std::map<std::string, std::string> sys;
  sys["os"] = I.d.sys.os; sys["cpu"] = I.d.sys.cpu; sys["gpu"] = I.d.sys.gpu; sys["power_plan"] = I.d.sys.powerPlan; sys["app"] = I.d.sys.appHash; sys["monitors"] = I.d.sys.monitorTopology;
  sb.setSystemInfo(sys);
  std::string ad; for (auto& kv : I.adaptHistory) ad += std::to_string(kv.first / kNsPerMs) + " ms  " + kv.second + "\n";
  sb.addTextSection("adaptation", "Adaptation history", "adaptation.txt", ad.empty() ? std::string("no adaptation events\n") : ad);
  { std::lock_guard<std::mutex> l(I.benchMu); if (!I.lastBenchJson.empty()) sb.addTextSection("benchmark", "Last benchmark result", "benchmark.json", I.lastBenchJson); }
}
std::vector<BundleCategory> AppController::supportBundlePreview() {
  std::lock_guard<std::mutex> l(p_->diagMu); p_->journal->flush();
  SupportBundle sb(*p_->dfiles, *p_->clk); fillBundle(*p_, sb); return sb.preview();
}
Status AppController::exportSupportBundle(const std::string& dest, const std::set<std::string>& approved, BundleReport* rep) {
  std::lock_guard<std::mutex> l(p_->diagMu); p_->journal->flush(); p_->logger->flush();
  SupportBundle sb(*p_->dfiles, *p_->clk); fillBundle(*p_, sb); sb.preview();
  return sb.exportZip(dest, approved, rep);
}

}} // namespace ss::app
