// AppController: render thread (frame pacing, per-output composition, newest-wins mailboxes), camera, media, display routes.
#include "core/app/controller_impl.h"
#include <algorithm>
#include <cmath>

namespace ss { namespace app {

using SClock = std::chrono::steady_clock;

std::shared_ptr<FrameBox> AppController::Impl::boxFor(const std::string& id) {
  std::lock_guard<std::mutex> l(frameMu);
  auto& p = frameBoxes[id]; if (!p) p = std::make_shared<FrameBox>(); return p;
}

std::shared_ptr<Frame> AppController::Impl::acquire(std::vector<std::shared_ptr<Frame>>& pool, int w, int h) {
  for (auto& p : pool) if (p.use_count() == 1) { if (p->w != w || p->h != h) p->resize(w, h); return p; }
  if (pool.size() < 8) { auto f = std::make_shared<Frame>(); f->resize(w, h); pool.push_back(f); return f; }
  return nullptr;                                   // every pooled frame is still held by a slow consumer: drop, never grow
}

std::string AppController::Impl::programId(const ShowView& v) const {
  { Impl* self = const_cast<Impl*>(this); std::lock_guard<std::mutex> l(self->progMu); if (!programOutput.empty()) return programOutput; }
  for (const auto& o : v.doc->outputs) if (o.role == show::OutputRole::Program) return o.logicalId;
  for (const auto& o : v.doc->outputs) if (o.role == show::OutputRole::Audience) return o.logicalId;
  return std::string();
}

void AppController::Impl::renderLoop() {
  while (!stopRender.load()) {
    renderStep(clk->nowNs());
    Ns next = std::min(nextRender, nextPreview);
    Ns now = clk->nowNs();
    if (next <= now) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    else sleepUntil(next, stopRender);
  }
}

void AppController::Impl::syncSlots(const RouteTable& t) {
  std::set<std::string> keep;
  for (const auto& r : t.routes) {
    if (!r.rendersFrames()) continue;
    keep.insert(r.logicalId);
    int w = r.w > 0 ? r.w : cfg.programW, h = r.h > 0 ? r.h : cfg.programH;
    OutSlot& s = slots[r.logicalId];
    s.id = r.logicalId;
    if (s.r && s.ok && s.w == w && s.h == h) { s.gen = r.generation; continue; }
    s.w = w; s.h = h; s.gen = r.generation; s.pool.clear(); s.clocks.reset();
    s.r.reset(new Renderer());
    float scale = std::min(1.0f, std::sqrt((float)cfg.maxInternalPixels / (float)std::max(1, w * h)));
    scale = std::max(0.1f, scale);
    Status st = s.r->configure(w, h, scale);
    while (st.code == Err::ResourceExhausted && scale > 0.15f) { scale *= 0.75f; st = s.r->configure(w, h, scale); }
    s.baseScale = scale; s.ok = st.ok();
    if (d.text) s.r->setTextRasterizer(d.text);
    if (!st.ok()) { logWarn("render", "output '" + r.logicalId + "' renderer configure failed: " + st.detail); setWarning("render:" + r.logicalId, "Output '" + r.logicalId + "' cannot render at " + std::to_string(w) + "x" + std::to_string(h) + ": " + st.detail, false); }
    else clearWarning("render:" + r.logicalId);
  }
  for (auto it = slots.begin(); it != slots.end();) {
    if (!keep.count(it->first)) { boxFor(it->first)->box.put(FramePtr()); it = slots.erase(it); } else ++it;
  }
  // routes that stopped rendering (disconnected, unassigned): clear their newest frame so no window shows stale content
  for (const auto& r : t.routes) if (!r.rendersFrames()) boxFor(r.logicalId)->box.put(FramePtr());
}

media::MediaHandlePtr AppController::Impl::handleFor(const std::string& hash) {
  if (hash.empty() || !mediaMgr) return nullptr;
  std::lock_guard<std::mutex> l(mediaMu);
  Ns now = clk->nowNs();
  auto it = handles.find(hash);
  if (it != handles.end()) {
    if (it->second.h && it->second.h->state() == media::MediaHandle::State::Failed && now - it->second.tried > 2 * kNsPerSec) {   // bounded retry
      mediaMgr->invalidate(hash); it->second.h = mediaMgr->request(hash); it->second.tried = now;
    }
    return it->second.h;
  }
  HandleEntry e; e.h = mediaMgr->request(hash); e.tried = now; handles[hash] = e;
  return e.h;
}

std::shared_ptr<const Frame> AppController::Impl::mediaFrame(const std::string& hash, Ns now, Ns startNs) {
  auto h = handleFor(hash);
  return h ? h->frame(std::max<Ns>(0, now - startNs)) : nullptr;
}

void AppController::Impl::registerMedia(const show::ShowDocument& doc) {
  if (!mediaMgr) return;
  { std::lock_guard<std::mutex> l(mediaMu); handles.clear(); }
  std::string root; { std::lock_guard<std::mutex> l(pathMu); root = mediaRoot; }
  size_t preload = 0;
  for (const auto& m : doc.media) {
    media::AssetSpec sp; sp.id = m.hash; sp.path = root.empty() ? m.relPath : persist::joinPath(root, m.relPath);
    Status s = mediaMgr->registerAsset(sp);
    if (!s.ok()) { logWarn("media", "asset registration refused: " + s.detail); continue; }
    if (preload < 12) { handleFor(m.hash); ++preload; }       // warm the cache off the live path; the rest load on demand
  }
}

void AppController::Impl::pollCamera(Ns now, bool) {
  if (!d.camera) return;
  bool open; { std::lock_guard<std::mutex> l(camMu); open = camOpen; }
  if (!open) { camFrame.reset(); camLastNs = -1; return; }
  Frame tmp;
  if (d.camera->latest(tmp) && tmp.w > 0 && tmp.h > 0 && tmp.px.size() >= (size_t)tmp.w * tmp.h) {
    if (!camFrame) camFrame = std::make_shared<Frame>();
    *camFrame = std::move(tmp); camLastNs = now;
  }
}

void AppController::Impl::renderStep(Ns now) {
  if (!renderInit) { renderInit = true; nextRender = now; nextPreview = now; }
  const bool doMain = now >= nextRender, doPrev = now >= nextPreview;
  if (!doMain && !doPrev) return;
  const Ns period = (Ns)(1e11 / std::max(500, sceneFpsX100.load()));
  const Ns prevPeriod = (Ns)(1e11 / std::max(100, previewFpsX100.load()));
  ShowViewPtr v;
  if (!viewBox.get(v) || !v) { if (doMain) nextRender = now + period; if (doPrev) nextPreview = now + prevPeriod; return; }
  std::shared_ptr<const RouteTable> tbl = router.table();
  if (tbl->generation != slotsGen) { syncSlots(*tbl); slotsGen = tbl->generation; }
  if (v->docGeneration != lastDocGen) { lastDocGen = v->docGeneration; for (auto& kv : slots) kv.second.clocks.reset(); previewClocks.reset(); curMediaRef.clear(); }
  CameraPublication cp; CameraTreatments ct; { std::lock_guard<std::mutex> l(camMu); cp = camPub; ct = camT; }
  if (doMain || doPrev) pollCamera(now, cp.published);
  const Frame* camPtr = (camFrame && camLastNs >= 0 && now - camLastNs <= cfg.cameraFreezeNs) ? camFrame.get() : nullptr;   // REQ-S11
  std::shared_ptr<const Frame> mediaF;
  if (!v->snap.cueMediaRef.empty()) {
    if (v->snap.cueMediaRef != curMediaRef) { curMediaRef = v->snap.cueMediaRef; curMediaStart = now; }
    mediaF = mediaFrame(curMediaRef, now, curMediaStart);
  } else curMediaRef.clear();

  if (doMain) {
    const auto t0 = SClock::now();
    const Ns deadline = nextRender + period;
    const std::string progId = programId(*v);
    std::map<std::string, FramePtr> shared;
    std::vector<std::shared_ptr<const Frame>> hold;
    const float sc = (float)scaleX1000.load() / 1000.0f;
    for (const auto& desc : tbl->routes) {
      if (!desc.rendersFrames()) continue;
      auto sit = slots.find(desc.logicalId); if (sit == slots.end() || !sit->second.ok) continue;
      OutSlot& s = sit->second;
      ComposeExtras x;
      x.elapsedNs = now - showStartNs; x.generation = 0; x.refBorder = desc.diagnostics; x.media = mediaF.get(); x.camera = camPtr;
      x.cameraRouted = cp.published && (cp.outputs.empty() || std::find(cp.outputs.begin(), cp.outputs.end(), desc.logicalId) != cp.outputs.end());
      x.cameraTreatments = ct; x.internalScale = sc < 0.999f ? s.baseScale * sc : 0.0f; x.shedLevel = trailsReduced.load() ? 2 : 0;
      if (desc.outputIndex >= 0 && desc.outputIndex < (int)v->doc->outputs.size()) {
        const show::Output& oc = v->doc->outputs[(size_t)desc.outputIndex];
        if (oc.overlay.branding && !oc.overlay.brandingRef.empty()) { auto bf = mediaFrame(oc.overlay.brandingRef, now, showStartNs); if (bf) { hold.push_back(bf); x.branding = bf.get(); } }
      }
      RenderState rs = composeState(*v, desc.outputIndex, s.clocks, x);
      std::string key = compositionKey(rs, s.w, s.h);
      FramePtr f;
      auto sh = shared.find(key);
      if (sh != shared.end()) f = sh->second;                  // duplicate routes share ONE composition
      else {
        auto fr = acquire(s.pool, s.w, s.h);
        if (fr) { s.r->render(*fr, rs); fr->pts = now; fr->generation = 0; router.finalize(desc, *fr); f = fr; shared[key] = f; }
      }
      auto bx = boxFor(desc.logicalId);
      if (!f) { ++bx->dropped; ++framesDroppedPool; metrics.drop(DropCat::Render); continue; }
      bx->box.put(f); ++bx->published;
      if (desc.logicalId == progId) {
        progBox.put(f);
        auto r = rec.get();
        if (r && r->profile().container != RecContainer::Wav && r->state() == RecState::Recording) {
          const VideoProfile& vp = r->profile().video; FramePtr rf = f;
          if (f->w != vp.width || f->h != vp.height) { auto scl = std::make_shared<Frame>(); fitScaleFrame(*f, vp.width, vp.height, *scl); rf = scl; }
          r->pushVideo(rf);
        }
      }
    }
    Ns after = clk->nowNs();
    ++framesRendered; ++deadlinesEval;
    bool missed = after > deadline;
    if (missed) ++deadlinesMissed;
    metrics.deadline(missed);
    metrics.renderTime((Ns)std::chrono::duration_cast<std::chrono::nanoseconds>(SClock::now() - t0).count());
    metrics.framePresented();
    nextRender += period;
    if (after > nextRender + period) nextRender = after;      // skipped frames are not queued: animation uses elapsed time
  }

  if (doPrev) {
    if (!previewR) {
      previewR.reset(new Renderer());
      float scale = std::min(1.0f, std::sqrt((float)cfg.maxInternalPixels / (float)std::max(1, cfg.previewW * cfg.previewH)));
      previewR->configure(cfg.previewW, cfg.previewH, std::max(0.1f, scale));
      if (d.text) previewR->setTextRasterizer(d.text);
    }
    int idx = v->snap.target;
    if (idx < 0) { for (size_t i = 0; i < v->doc->outputs.size(); ++i) if (v->doc->outputs[i].role != show::OutputRole::Controls) { idx = (int)i; break; } }
    ComposeExtras x; x.elapsedNs = now - showStartNs; x.media = mediaF.get(); x.camera = camPtr; x.cameraRouted = cp.published; x.cameraTreatments = ct;
    x.allowOverlays = false; x.forceBlackout = router.blackout(); x.shedLevel = trailsReduced.load() ? 2 : 0;
    RenderState rs = composeState(*v, idx, previewClocks, x);
    auto fr = acquire(previewPool, cfg.previewW, cfg.previewH);
    if (fr) { previewR->render(*fr, rs); fr->pts = now; previewBox.put(fr); }
    if (camFrame && camLastNs >= 0 && now - camLastNs <= cfg.cameraFreezeNs * 4) camPreviewBox.put(std::make_shared<Frame>(*camFrame));
    else camPreviewBox.put(FramePtr());
    nextPreview += prevPeriod;
    if (nextPreview < now) nextPreview = now + prevPeriod;
  }
}

// ===================================================================================================================
// camera / media / display public API
// ===================================================================================================================
std::vector<CameraInfo> AppController::cameras() { return p_->d.camera ? p_->d.camera->enumerate() : std::vector<CameraInfo>(); }
Status AppController::selectCamera(const CameraInfo& c, CameraFormat want) {
  Impl& I = *p_;
  if (!I.d.camera) return Status::Fail(Err::UnsupportedCapability, "no camera backend is available");
  I.d.camera->close();
  { std::lock_guard<std::mutex> l(I.camMu); I.camPub = CameraPublication(); I.camOpen = false; }    // a new camera is NEVER published implicitly
  Status s = I.d.camera->open(c.id, want);
  if (s.ok()) {
    { std::lock_guard<std::mutex> l(I.camMu); I.camInfo = c; I.camOpen = true; }
    I.settingsPut("camera", c.id); I.settingsPut("cameraName", c.name); I.settingsPut("setup.cameraPreviewed", "1"); I.saveSettingsNow();
    I.jrec(JournalKind::Device, "camera", "preview opened (not published)");
  }
  return s;
}
void AppController::closeCamera() {
  if (p_->d.camera) p_->d.camera->close();
  std::lock_guard<std::mutex> l(p_->camMu); p_->camOpen = false; p_->camPub = CameraPublication();
}
void AppController::setCameraTreatments(const CameraTreatments& t) { std::lock_guard<std::mutex> l(p_->camMu); p_->camT = t; }
void AppController::publishCamera(const CameraPublication& pub) {
  { std::lock_guard<std::mutex> l(p_->camMu);
    if (pub.published && !p_->camOpen) return;                // nothing to publish
    p_->camPub = pub; }
  p_->jrec(JournalKind::Device, "camera", pub.published ? "published to audience/broadcast scenes" : "unpublished");
}
CameraPublication AppController::cameraPublication() const { std::lock_guard<std::mutex> l(p_->camMu); return p_->camPub; }

void AppController::setMediaRoot(const std::string& dir) {
  { std::lock_guard<std::mutex> l(p_->pathMu); p_->mediaRoot = dir; }
  std::shared_ptr<const show::ShowDocument> doc = document();
  if (doc) p_->registerMedia(*doc);
}
std::vector<std::string> AppController::missingMedia() const {
  std::vector<std::string> out;
  std::shared_ptr<const show::ShowDocument> doc = document();
  std::string root; { std::lock_guard<std::mutex> l(p_->pathMu); root = p_->mediaRoot; }
  std::lock_guard<std::mutex> l(p_->mediaMu);
  for (const auto& m : doc->media) {
    std::string path = root.empty() ? m.relPath : persist::joinPath(root, m.relPath);
    bool missing = !p_->pfiles->exists(path);
    auto it = p_->handles.find(m.hash);
    if (!missing && it != p_->handles.end() && it->second.h && it->second.h->state() == media::MediaHandle::State::Failed) missing = true;
    if (missing) out.push_back(m.hash);
  }
  return out;
}

std::vector<DisplayInfo> AppController::displays() { return p_->d.displays ? p_->d.displays->enumerate() : std::vector<DisplayInfo>(); }
void AppController::refreshDisplays() { p_->refreshDisplaysNow(); }
Status AppController::assignOutput(const std::string& id, const DisplayInfo& d) { Status s = p_->router.assign(id, d); if (s.ok()) p_->saveSettingsNow(); return s; }
Status AppController::assignOutputById(const std::string& id, const std::string& did) { Status s = p_->router.assignById(id, did); if (s.ok()) p_->saveSettingsNow(); return s; }
Status AppController::unassignOutput(const std::string& id) { Status s = p_->router.unassign(id); if (s.ok()) p_->saveSettingsNow(); return s; }
Status AppController::setControlsDisplay(const std::string& id) { p_->router.setControlsDisplay(id); return p_->saveSettingsNow(); }
void AppController::setOutputDiagnostics(const std::string& id, bool on) { p_->router.setDiagnostics(id, on); }
Status AppController::setProgramOutput(const std::string& id) {
  std::shared_ptr<const show::ShowDocument> doc = document();
  if (!id.empty()) {
    bool ok = false; for (const auto& o : doc->outputs) if (o.logicalId == id && o.role != show::OutputRole::Controls) ok = true;
    if (!ok) return Status::Fail(Err::InvalidArgument, "unknown or non-visual output '" + id + "'");
  }
  std::lock_guard<std::mutex> l(p_->progMu); p_->programOutput = id; return Status::Ok();
}

}} // namespace ss::app
