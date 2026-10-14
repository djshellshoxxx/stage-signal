// AppController: audio analysis worker, MIDI wiring, lighting worker, recording/broadcast integration.
#include "core/app/controller_impl.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ss { namespace app {

using json = nlohmann::json;

// ===================================================================================================================
// audio capture -> analysis worker
// ===================================================================================================================
void AppController::Impl::onAudioBlock(AudioBlockPtr b) {              // capture thread: copy into a bounded queue only
  if (!b || stopAnalysis.load()) return;
  if (!capQ.push(b)) { ++capOverruns; pendingDisc = true; }
  anCv.notify_one();
}

void AppController::Impl::onAudioStatus(const AudioStatus& s) {        // any thread: publish + flag
  audioStatBox.put(s);
  audioAvail = (s.state == AudioState::Running);
  audioAvailDirty = true;
  wake();
}

void AppController::Impl::analysisLoop() {
  while (!stopAnalysis.load()) {
    size_t n = analysisStep(32);
    if (n == 0) {
      std::unique_lock<std::mutex> l(anMu);
      anCv.wait_for(l, std::chrono::milliseconds(5), [&] { return stopAnalysis.load() || capQ.blocks() > 0; });
    }
  }
}

size_t AppController::Impl::analysisStep(int maxBlocks) {
  AudioBlockPtr b; int n = 0; bool any = false;
  std::shared_ptr<RecordingSession> r = rec.get();
  const bool recOn = r && r->state() == RecState::Recording;
  const bool bcOn = castActive.load();
  const bool bus = recOn || bcOn || cfg.dynamicsAlwaysOn;
  bool telem = false;
  while (n < maxBlocks && capQ.pop(b)) {
    ++n;
    if (!analyzer || b->rate != aRate || b->channels != aCh) {
      aRate = b->rate; aCh = std::max<uint32_t>(1, b->channels);
      analyzer.reset(new Analyzer(aRate, aCh)); dynamics.reset(new Dynamics(aRate, aCh)); dynDirty = true;
    }
    if (dynDirty.exchange(false)) { DynamicsConfig c; { std::lock_guard<std::mutex> l(dynMu); c = dynCfg; } dynamics->stageConfig(c); }
    if (pendingDisc.exchange(false) && !(b->flags & AF_Discontinuity)) {      // capture overflow => discontinuity (spec §8)
      AudioBlock cp = *b; cp.flags |= AF_Discontinuity; analyzer->process(cp);
    } else analyzer->process(*b);
    any = true;
    if (bus) {
      auto ob = std::make_shared<AudioBlock>();
      ob->samples.resize(b->samples.size());
      dynamics->process(*b, *ob);
      if (recOn) r->pushAudio(ob);
      if (bcOn) castQ.push(ob);
      telem = true;
    }
  }
  if (any) {
    FeatureSnapshot f = analyzer->snapshot(clk->nowNs());
    featBox.put(f);
    if (telem) {
      DynamicsTelemetry t = dynamics->telemetry();
      procPeakDb = t.outPeakDb; lastGrDb = t.limGrDb + t.compGrDb;
      if (recOn) r->noteBlockTelemetry(f.inputNearFullScale, f.possibleClipping, t.limGrDb > 0.1f, t.maxLimGrDb);
    }
  }
  return (size_t)n;
}

Status AppController::Impl::openAudio(const AudioOpenParams& p, bool save) {
  if (!d.audio) return Status::Fail(Err::UnsupportedCapability, "no audio capture backend is available");
  { std::lock_guard<std::mutex> l(audioMu); audioParams = p; audioChosen = true; }
  Status s = d.audio->open(p);
  AudioStatus st = d.audio->status();
  { std::lock_guard<std::mutex> l(audioMu); audioName = st.deviceName; }
  audioStatBox.put(st);
  audioAvail = (st.state == AudioState::Running) && s.ok(); audioAvailDirty = true; wake();
  if (s.ok() && save) {
    settingsPut("audioInput", p.deviceId); settingsPut("audioInputName", st.deviceName); settingsPut("audioRoute", std::to_string(p.channelRoute));
    saveSettingsNow();
  }
  if (s.ok()) { clearWarning("audio_open"); clearWarning("audio_lost"); }
  return s;
}

std::vector<AudioDeviceInfo> AppController::audioDevices() { return p_->d.audio ? p_->d.audio->enumerate() : std::vector<AudioDeviceInfo>(); }
Status AppController::selectAudio(const AudioOpenParams& p) { return p_->openAudio(p, true); }
Status AppController::reopenAudio() {
  AudioOpenParams prm; bool chosen;
  { std::lock_guard<std::mutex> l(p_->audioMu); prm = p_->audioParams; chosen = p_->audioChosen; }
  if (!chosen) return Status::Fail(Err::InvalidArgument, "no audio device has been chosen yet");
  if (p_->d.audio) p_->d.audio->close();
  return p_->openAudio(prm, false);                  // the SAME device id only; never another endpoint
}
void AppController::closeAudio() { if (p_->d.audio) { p_->d.audio->close(); AudioStatus st = p_->d.audio->status(); p_->audioStatBox.put(st); p_->audioAvail = false; p_->audioAvailDirty = true; p_->wake(); } }
void AppController::setDynamics(const DynamicsConfig& c) { { std::lock_guard<std::mutex> l(p_->dynMu); p_->dynCfg = c.clamped(); } p_->dynDirty = true; }
DynamicsConfig AppController::dynamics() const { std::lock_guard<std::mutex> l(p_->dynMu); return p_->dynCfg; }
void AppController::acknowledgeGainNote(bool ack) { p_->settingsPut("setup.gainAck", ack ? "1" : ""); p_->saveSettingsNow(); }

// ===================================================================================================================
// MIDI
// ===================================================================================================================
Status AppController::Impl::openMidiInput(const MidiPortInfo& p, bool save) {
  if (!d.midiIn) return Status::Fail(Err::UnsupportedCapability, "no MIDI backend is available");
  if (midiInIdx >= 0) d.midiIn->closeInput();
  int idx = -1;
  Status s = mrouter->attach(*d.midiIn, p, &idx);
  if (idx >= 0) midiInIdx = idx;
  midiInId = p.stableId;
  if (s.ok()) { midiInLost = false; clearWarning("midi_in"); }
  if (s.ok() && save) { settingsPut("midiIn", p.stableId); saveSettingsNow(); }
  return s;
}
Status AppController::Impl::openMidiOutput(const MidiPortInfo& p, bool save) {
  if (!d.midiOut) return Status::Fail(Err::UnsupportedCapability, "no MIDI backend is available");
  Status s = d.midiOut->openOutput(p.stableId);
  if (!s.ok()) return s;
  midiOutId = p.stableId; midiOutLost = false; midiOutOpen = true;
  if (!mout) {
    mout.reset(new MidiOutputScheduler(*clk, *d.midiOut, moutCfg));
    mout->setTempoSource([this] { return effBpm.load(); });
    mout->setClockEnabled(midiClock);
    if (!cfg.manualPump) mout->start();
  } else mout->notePortRestored();
  graph.removeRoute("out:feedback");
  if (!midiInId.empty() && (midiInId == p.stableId || (d.midiIn == d.midiOut))) graph.declareSameDevice(midiInId, p.stableId);
  MidiRoute rt; rt.id = "out:feedback"; rt.kind = RouteKind::ActionToOutput; rt.outPort = p.stableId;
  graph.addRoute(rt);                                    // feedback-risk routes start disabled until explicitly enabled
  clearWarning("midi_out");
  if (save) { settingsPut("midiOut", p.stableId); saveSettingsNow(); }
  return Status::Ok();
}
void AppController::Impl::restoreSavedMidi() {
  std::string in = settingsGet("midiIn"), out = settingsGet("midiOut");
  if (!in.empty() && d.midiIn) for (auto& pi : d.midiIn->enumerate()) if (pi.stableId == in && pi.input) { openMidiInput(pi, false); break; }
  if (!out.empty() && d.midiOut) for (auto& pi : d.midiOut->enumerate()) if (pi.stableId == out && !pi.input) { openMidiOutput(pi, false); break; }
}
std::vector<MidiPortInfo> AppController::midiPorts() {
  std::vector<MidiPortInfo> v;
  if (p_->d.midiIn) { auto a = p_->d.midiIn->enumerate(); v.insert(v.end(), a.begin(), a.end()); }
  if (p_->d.midiOut && p_->d.midiOut != p_->d.midiIn) { auto a = p_->d.midiOut->enumerate(); v.insert(v.end(), a.begin(), a.end()); }
  return v;
}
Status AppController::selectMidiInput(const MidiPortInfo& pi) { return p_->openMidiInput(pi, true); }
Status AppController::selectMidiOutput(const MidiPortInfo& pi) { return p_->openMidiOutput(pi, true); }
Status AppController::reconnectMidi() {
  Status res = Status::Ok();
  if (p_->midiInIdx >= 0 && p_->mrouter) { Status s = p_->mrouter->reconnect(p_->midiInIdx); if (!s.ok()) res = s; else { p_->clearWarning("midi_in"); p_->jrec(JournalKind::Device, "midi:input", "reconnected by operator"); } }
  if (p_->d.midiOut && !p_->midiOutId.empty()) {
    Status s = p_->d.midiOut->openOutput(p_->midiOutId);
    if (s.ok()) { if (p_->mout) p_->mout->notePortRestored(); p_->midiOutLost = false; p_->clearWarning("midi_out"); }
    else res = s;
  }
  return res;
}
void AppController::setMidiClockEnabled(bool on) { p_->midiClock = on; if (p_->mout) p_->mout->setClockEnabled(on); }
void AppController::setMidiFeedback(const std::vector<MidiFeedbackRule>& rules) { std::lock_guard<std::mutex> l(p_->fbMu); p_->fbRules = rules; }
void AppController::setMidiOutConfig(const MidiOutConfig& c) { p_->moutCfg = c; if (p_->mout) p_->mout->setConfig(c); }

// ===================================================================================================================
// lighting
// ===================================================================================================================
void AppController::Impl::lightingLoop() {
  while (!stopLight.load()) {
    lightingStep(clk->nowNs());
    sleepUntil(clk->nowNs() + 5 * kNsPerMs, stopLight);
  }
}

void AppController::Impl::lightingStep(Ns) {
  ShowViewPtr v; if (!viewBox.get(v) || !v) return;
  std::lock_guard<std::mutex> l(lightMu);
  LightingEngine* e = live ? live.get() : dry.get();
  if (!e) return;
  if (v->snap.cueLightingRef != lastLookRef) {
    lastLookRef = v->snap.cueLightingRef;
    auto it = looks.find(lastLookRef);
    if (lastLookRef.empty() || it == looks.end()) e->clearCue(); else e->setCue(it->second);
  }
  e->submitFeatures(v->features);
  e->pump();
  if (live) {
    EngineStatus st = live->status();
    if (st.state != ArmState::Armed || st.faulted) liveFaulted = true;
  } else if (dry && dry->dryRunSink()) previewDatagrams = dry->dryRunSink()->wouldSend();
}

Status AppController::Impl::buildLighting(const show::ShowDocument& doc) {
  ProfileLibrary l = ProfileLibrary::builtin();
  std::string warn;
  for (const auto& txt : doc.fixtureProfiles) { Status s = l.addJson(txt); if (!s.ok()) warn += "profile: " + s.detail + "; "; }
  json env; env["schema"] = "stagesignal.patch"; env["version"] = 1; env["fixtures"] = json::array();
  for (const auto& txt : doc.patch) { json j = json::parse(txt, nullptr, false); if (j.is_object()) env["fixtures"].push_back(j); else warn += "patch entry is not an object; "; }
  Patch pt; Status ps = Patch::fromJson(env.dump(), pt);
  if (!ps.ok()) { warn += ps.detail; pt = Patch(); }
  int minU = lcfg.protocol == LightProtocol::Sacn ? (int)sacn::kMinUniverse : 0, maxU = lcfg.protocol == LightProtocol::Sacn ? (int)sacn::kMaxUniverse : (int)artnet::kMaxPortAddress;
  PatchReport rep = pt.validate(l, minU, maxU);
  Status err;
  std::unique_ptr<LightingEngine> eng = LightingEngine::makeDryRun(*clk, lcfg, &err);
  if (!eng) { EngineConfig safe; eng = LightingEngine::makeDryRun(*clk, safe, &err); warn += "lighting config invalid, preview uses defaults: " + err.detail + "; "; }
  bool ok = false;
  if (eng && !pt.entries.empty() && rep.ok()) {
    Status as = eng->applyPatch(pt, l, &rep);
    if (as.ok()) {
      eng->setAudioMappings(audioMaps);
      // dry-run engines own a DryRunSink and can never reach a real socket (REQ-L03), so arming the PREVIEW engine is safe
      Status os = eng->openTransport();
      ArmToken tk; if (os.ok() && eng->requestArmToken(tk).ok()) ok = eng->arm(tk).ok();
    } else warn += as.detail;
  }
  { std::lock_guard<std::mutex> g(lightMu); dry = std::move(eng); lib = l; patch = pt; patchReport = rep; patchOk = rep.ok(); lastLookRef.clear(); previewDatagrams = 0; }
  if (!warn.empty()) { setWarning("lighting_patch", "Lighting patch: " + warn, false); logWarn("lighting", warn); } else clearWarning("lighting_patch");
  if (!rep.errors.empty()) setWarning("lighting_patch", "Lighting patch invalid: " + rep.errors[0], false);
  (void)ok;
  return Status::Ok();
}

Status AppController::Impl::doArmLighting() {
  if (!d.lightingSink) return Status::Fail(Err::UnsupportedCapability, "no lighting output interface is available");
  if (liveArmed.load()) return Status::Ok();
  std::unique_ptr<LightingEngine> e; Status err;
  Patch pt; ProfileLibrary lb; EngineConfig lc;
  { std::lock_guard<std::mutex> l(lightMu);
    if (!patchOk || !patchReport.canArm() || patch.entries.empty()) return Status::Fail(Err::InvalidArgument, "the lighting patch is empty, invalid or has unresolved conflicts: fix it before arming");
    pt = patch; lb = lib; lc = lcfg; }
  e = LightingEngine::makeLive(*clk, lc, *d.lightingSink, &err);
  if (!e) return err;
  PatchReport rep; Status ps = e->applyPatch(pt, lb, &rep); if (!ps.ok()) return ps;
  e->setAudioMappings(audioMaps);
  Status os = e->openTransport(); if (!os.ok()) return os;
  ArmToken tk; Status ts = e->requestArmToken(tk); if (!ts.ok()) return ts;
  Status as = e->arm(tk); if (!as.ok()) return as;
  e->setLightingBlackout(lastSnap.lightingBlackoutLatch); e->setEmergencyBlackout(lastSnap.emergencyBlackoutLatch); e->setStrobeDisabled(lastSnap.strobeDisabled);
  { std::lock_guard<std::mutex> l(lightMu); live = std::move(e); lastLookRef.clear(); }
  liveArmed = true; liveFaulted = false;
  jrec(JournalKind::Device, "lighting", "armed by operator");
  logInfo("lighting", "DMX armed by explicit operator action");
  return Status::Ok();
}

void AppController::Impl::doDisarmLighting(const char* why) {
  std::unique_ptr<LightingEngine> old;
  { std::lock_guard<std::mutex> l(lightMu); old = std::move(live); }
  if (old) {
    old->disarm();                                       // graceful safe frames (+ sACN termination), then silent
    old->stop();
    old.reset();                                         // engine before sink
    if (d.lightingSink) d.lightingSink->close();
    jrec(JournalKind::Device, "lighting", std::string("disarmed: ") + why);
    logInfo("lighting", std::string("DMX disarmed: ") + why);
  }
  liveArmed = false;
}

void AppController::Impl::syncLightingLatches(const show::ShowSnapshot& s) {
  std::lock_guard<std::mutex> l(lightMu);
  for (LightingEngine* e : {dry.get(), live.get()}) if (e) {
    e->setLightingBlackout(s.lightingBlackoutLatch); e->setEmergencyBlackout(s.emergencyBlackoutLatch); e->setStrobeDisabled(s.strobeDisabled);
  }
}

Status AppController::setLightingInterface(const LightingInterface& li) {
  Impl& I = *p_;
  if (I.liveArmed.load()) return Status::Fail(Err::InvalidArgument, "disarm lighting before changing the interface");
  EngineConfig c = I.lcfg; c.protocol = li.protocol; c.host = li.host; c.nicAddr = li.nicAddr; c.sacnMulticast = li.sacnMulticast; c.allowBroadcast = li.allowBroadcast;
  Status v = LightingEngine::validateConfig(c, true);
  if (!v.ok()) return v;
  I.lcfg = c;
  I.settingsPut("lightingProtocol", li.protocol == LightProtocol::Sacn ? "sacn" : li.protocol == LightProtocol::UsbDmx ? "usb" : "artnet");
  I.settingsPut("lightingHost", li.host); I.settingsPut("lightingNic", li.nicAddr);
  I.settingsPut("lightingMulticast", li.sacnMulticast ? "1" : ""); I.settingsPut("lightingBroadcast", li.allowBroadcast ? "1" : "");
  I.settingsPut("setup.lighting", "1");
  { std::shared_ptr<const show::ShowDocument> doc = document(); I.buildLighting(*doc); }
  return I.saveSettingsNow();
}
void AppController::setLightingLooks(const std::map<std::string, ss::Cue>& l) { std::lock_guard<std::mutex> g(p_->lightMu); p_->looks = l; p_->lastLookRef.clear(); }
Status AppController::setLightingAudioMappings(const std::vector<AudioMapping>& m) {
  std::lock_guard<std::mutex> g(p_->lightMu);
  p_->audioMaps = m;
  Status res = Status::Ok();
  for (LightingEngine* e : {p_->dry.get(), p_->live.get()}) if (e) { Status s = e->setAudioMappings(m); if (!s.ok()) res = s; }
  return res;
}
uint64_t AppController::lightingDatagramsSent() const { return p_->d.lightingSink ? p_->d.lightingSink->datagramsSent() : 0; }
uint64_t AppController::lightingDatagramsPreviewed() const { return p_->previewDatagrams.load(); }
bool AppController::lightingArmed() const { return p_->liveArmed.load(); }

// ===================================================================================================================
// recording
// ===================================================================================================================
static std::string dynJson(const DynamicsConfig& c) {
  json j; j["bypass"] = c.bypass; j["trimDb"] = c.trimDb; j["compressorOn"] = c.compressorOn; j["thresholdDb"] = c.thresholdDb; j["ratio"] = c.ratio;
  j["attackMs"] = c.attackMs; j["releaseMs"] = c.releaseMs; j["kneeDb"] = c.kneeDb; j["makeupDb"] = c.makeupDb; j["limiterOn"] = c.limiterOn;
  j["ceilingDb"] = c.ceilingDb; j["limiterReleaseMs"] = c.limiterReleaseMs; return j.dump();
}

Status AppController::Impl::doStartRecording(ActionSource src) {
  if (src == ActionSource::Show || src == ActionSource::System) return Status::Fail(Err::PermissionDenied, "recording can be started only by an explicit operator action");
  IFileSystem* fs = d.recFs ? d.recFs : &stdFileSystem();
  IDiskInfo* disk = d.disk;
#if !defined(_WIN32)
  if (!disk) disk = &posixDiskInfo();
#endif
  if (!disk) return Status::Fail(Err::UnsupportedCapability, "disk space information is unavailable; recording cannot start");
  std::string folder; { std::lock_guard<std::mutex> l(setMu); folder = settings.recordingFolder; }
  if (folder.empty()) return Status::Fail(Err::InvalidArgument, "choose a recording folder in Setup before recording");
  AudioStatus ast; audioStatBox.get(ast);
  if (ast.state != AudioState::Running) return Status::Fail(Err::DeviceLost, "no audio input is running: recording needs the selected audio bus");
  std::string reason;
  if (!adapt.tryAddWork("recording", &reason)) return Status::Fail(Err::ResourceExhausted, "recording refused: " + reason);
  RecordingProfile prof; { std::lock_guard<std::mutex> l(recMu); prof = recProfile; }
  prof.audioRate = ast.rate; prof.audioChannels = std::min<uint32_t>(2, std::max<uint32_t>(1, ast.channels));
  RecordingRequest rq; rq.profile = prof; rq.dir = folder;
  { std::lock_guard<std::mutex> l(docMu); rq.showName = docPtr ? docPtr->name : std::string("show"); }
  rq.sessionId = "S" + std::to_string(clk->wallUnixMs());
  DynamicsConfig dc; { std::lock_guard<std::mutex> l(dynMu); dc = dynCfg; }
  rq.dynamics.enabled = true; rq.dynamics.bypassed = dc.bypass; rq.dynamics.settingsJson = dynJson(dc); rq.dynamics.delaySamples = ast.rate * 5 / 1000;
  rq.sources.audioPresent = true; rq.sources.audioRate = prof.audioRate; rq.sources.audioChannels = prof.audioChannels;
  rq.sources.videoPresent = prof.container != RecContainer::Wav; rq.sources.videoW = prof.video.width; rq.sources.videoH = prof.video.height;
  SessionDeps sd; sd.fs = fs; sd.disk = disk; sd.clock = clk; sd.encoders = encoders; sd.manualPump = false;
  auto ses = std::make_shared<RecordingSession>(sd);
  ses->setStateCallback([this](RecState s) { jrec(JournalKind::Record, recStateName(s)); });
  Status s = ses->start(rq);
  rec.set(ses);
  if (!s.ok()) { logWarn("recording", "start failed: " + s.detail); return s; }
  dynDirty = true;
  logInfo("recording", "started");
  return Status::Ok();
}

Status AppController::setRecordingFolder(const std::string& dir) {
  IFileSystem* fs = p_->d.recFs ? p_->d.recFs : &stdFileSystem();
  std::string why;
  if (dir.empty()) return Status::Fail(Err::InvalidArgument, "recording folder is empty");
  fs->makeDirs(dir);
  if (!fs->writableDir(dir, why)) return Status::Fail(Err::PermissionDenied, "recording folder is not writable: " + why);
  { std::lock_guard<std::mutex> l(p_->setMu); p_->settings.recordingFolder = dir; }
  return p_->saveSettingsNow();
}
void AppController::configureRecording(const RecordingProfile& pr) { std::lock_guard<std::mutex> l(p_->recMu); p_->recProfile = pr; p_->recProfileSet = true; }
RecordingStatus AppController::recordingStatus() const { auto r = p_->rec.get(); return r ? r->status() : RecordingStatus(); }

// ===================================================================================================================
// broadcast (explicit UserStartToken only) + feeder thread
// ===================================================================================================================
bool AppController::Impl::programCameraPublished() const {
  Impl* self = const_cast<Impl*>(this);
  std::lock_guard<std::mutex> l(self->camMu);
  if (!camPub.published) return false;
  if (camPub.outputs.empty()) return true;
  std::string pid;
  { ShowViewPtr v; if (self->viewBox.get(v) && v) pid = self->programId(*v); }
  return std::find(camPub.outputs.begin(), camPub.outputs.end(), pid) != camPub.outputs.end();
}
PublicationInputs AppController::Impl::makePublication() const {
  PublicationInputs pi;
  pi.programVideo = true; pi.cameraInProgram = programCameraPublished();
  { Impl* self = const_cast<Impl*>(this); std::lock_guard<std::mutex> l(self->camMu); if (pi.cameraInProgram) pi.cameraLabel = camInfo.name; }
  pi.audio = true; pi.dynamicsActive = true;
  auto r = rec.get(); pi.localRecordingActive = r && r->state() == RecState::Recording;
  return pi;
}

Status AppController::Impl::doStartBroadcast(UserStartToken&& tok, const PublicationInputs& pub) {
  if (!d.transports) return Status::Fail(Err::UnsupportedCapability, "broadcast transport is not available in this session");
  BroadcastSettings st; { std::lock_guard<std::mutex> l(bcMu); st = bcSettings; }
  std::string reason;
  if (!adapt.tryAddWork("broadcast", &reason)) return Status::Fail(Err::ResourceExhausted, "broadcast refused: " + reason);
  auto sess = bc.get();
  if (!sess) {
    BroadcastDeps bd; bd.secrets = d.secrets; bd.clock = clk; bd.factory = d.transports; bd.encoders = encoders;
    bd.log = [this](const std::string& line) { logInfo("broadcast", line); };
    bd.requestKeyframe = [this] { bcKeyReq = true; };
    sess = std::make_shared<BroadcastSession>(bd);
    sess->setStateCallback([this](BroadcastState s) { jrec(JournalKind::Broadcast, broadcastStateName(s)); });
    bc.set(sess);
  }
  if (d.secrets && !st.endpoint.streamKeyRef.empty()) {   // teach the redactor the literal so logs/journal/bundles never carry it
    std::string k; if (d.secrets->get(st.endpoint.streamKeyRef, k).ok()) { std::lock_guard<std::mutex> l(diagMu); redactor.addLiteral(k); }
    std::fill(k.begin(), k.end(), '\0');
  }
  if (!cfg.manualPump && !thCast.joinable()) {
    exCast = false; stopCast = false;
    std::shared_ptr<Impl> s = selfHold;
    if (s) thCast = std::thread([s] { s->castLoop(); s->exCast = true; });
    else exCast = true;
  }
  castQ.clear(); bcEnc.reset(); bcLastVideoPts = -1; bcSeq = 0; bcProgVer = progBox.version();
  castActive = true; dynDirty = true;
  Status s = sess->start(std::move(tok), st, pub);
  if (!s.ok()) { castActive = false; logWarn("broadcast", "start failed: " + s.detail); return s; }
  logInfo("broadcast", "start requested by operator");
  return Status::Ok();
}

void AppController::Impl::doStopBroadcast() {
  auto s = bc.get();
  if (s) s->stop();
  castActive = false;
  castQ.clear();
}

void AppController::Impl::castLoop() {
  while (!stopCast.load()) {
    castStep(clk->nowNs());
    sleepUntil(clk->nowNs() + 10 * kNsPerMs, stopCast);
  }
}

void AppController::Impl::castStep(Ns) {
  auto s = bc.get(); if (!s) return;
  s->tick();
  BroadcastState st = s->state();
  if (st == BroadcastState::Off || st == BroadcastState::Error) { if (castActive.load()) { castActive = false; castQ.clear(); } bcEnc.reset(); return; }
  BroadcastSettings set; { std::lock_guard<std::mutex> l(bcMu); set = bcSettings; }
  AudioBlockPtr b; int n = 0;
  while (n++ < 64 && castQ.pop(b)) {
    auto pk = std::make_shared<MediaPacket>();
    pk->kind = PacketKind::Audio; pk->codec = PacketCodec::PcmF32; pk->pts = b->captureNs; pk->frames = (uint32_t)b->frames();
    pk->data.resize(b->samples.size() * sizeof(float));
    if (!b->samples.empty()) std::memcpy(pk->data.data(), b->samples.data(), pk->data.size());
    pk->seq = ++bcSeq;
    s->submit(pk);
  }
  FramePtr f;
  if (progBox.version() != bcProgVer && progBox.get(f) && f) {
    bcProgVer = progBox.version();
    Ns minGap = (Ns)(1e9 / std::max(1, set.fps));
    if (bcLastVideoPts < 0 || f->pts - bcLastVideoPts >= minGap - minGap / 4) {
      if (!bcEnc) {
        VideoProfile vp; vp.codec = set.videoCodec; vp.width = set.width; vp.height = set.height; vp.fpsNum = set.fps; vp.fpsDen = 1; vp.bitrateKbps = set.videoKbps;
        Status es = encoders->create(vp, bcEnc);
        if (!es.ok()) { bcEnc.reset(); logWarn("broadcast", "encoder unavailable: " + es.detail); return; }
      }
      const Frame* src = f.get();
      if (f->w != set.width || f->h != set.height) { fitScaleFrame(*f, set.width, set.height, bcScaled); src = &bcScaled; }
      EncodedPacket ep; bool have = false;
      Status es = bcEnc->encode(*src, f->pts, ep, have);
      if (es.ok() && have) {
        auto pk = std::make_shared<MediaPacket>();
        pk->kind = PacketKind::Video; pk->codec = set.videoCodec == VideoCodec::MJPEG ? PacketCodec::Mjpeg : PacketCodec::H264;
        pk->pts = ep.pts; pk->key = ep.key; pk->data = std::move(ep.data); pk->seq = ++bcSeq;
        s->submit(pk);
        bcLastVideoPts = f->pts; bcKeyReq = false;
      }
    }
  }
}

Status AppController::configureBroadcast(const BroadcastSettings& s) {
  std::vector<std::string> warn;
  Status v = validateSettings(s, &warn);
  if (!v.ok()) return v;
  { std::lock_guard<std::mutex> l(p_->bcMu); p_->bcSettings = s; }
  std::string js = exportSettingsJson(s);
  p_->pfiles->createDirs(p_->cfg.dataDir);
  auto w = persist::writeFileAll(*p_->pfiles, persist::joinPath(p_->cfg.dataDir, "broadcast.json"), js.data(), js.size());
  return w;
}
BroadcastSettings AppController::broadcastSettings() const { std::lock_guard<std::mutex> l(p_->bcMu); return p_->bcSettings; }
Status AppController::storeSecret(const std::string& label, const std::string& secret) {
  if (!p_->d.secrets) return Status::Fail(Err::UnsupportedCapability, "no secret store is available");
  if (!show::validId(label)) return Status::Fail(Err::InvalidArgument, "secret label must be an identifier");
  Status s = p_->d.secrets->put(label, secret);
  if (!s.ok()) return Status::Fail(s.code, "secret store refused the value");        // never echo the secret
  { std::lock_guard<std::mutex> l(p_->diagMu); p_->redactor.addLiteral(secret); }
  { std::lock_guard<std::mutex> l(p_->setMu); p_->settings.secretRefs[label] = label; }
  return p_->saveSettingsNow();
}
BroadcastStatus AppController::broadcastStatus() const { auto b = p_->bc.get(); return b ? b->status() : BroadcastStatus(); }

}} // namespace ss::app
