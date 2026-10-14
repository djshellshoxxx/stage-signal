#include "core/app/render_compose.h"
#include <algorithm>
#include <cstring>

namespace ss { namespace app {

Ns SceneClocks::elapsed(int scene, Ns now) {
  auto it = start_.find(scene);
  if (it == start_.end()) { start_[scene] = now; return 0; }
  return now >= it->second ? now - it->second : 0;
}
void SceneClocks::keepOnly(const std::set<int>& vis) {
  for (auto it = start_.begin(); it != start_.end();) { if (!vis.count(it->first)) it = start_.erase(it); else ++it; }
}

std::u32string utf8ToU32(const std::string& s) {
  std::u32string o; size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = (unsigned char)s[i]; uint32_t cp = 0xFFFD; size_t len = 1;
    if (c < 0x80) { cp = c; }
    else {
      size_t need = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
      if (need && i + need <= n) {
        uint32_t v = c & (need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07); bool ok = true;
        for (size_t k = 1; k < need; ++k) { unsigned char cc = (unsigned char)s[i + k]; if ((cc & 0xC0) != 0x80) { ok = false; break; } v = (v << 6) | (cc & 0x3F); }
        if (ok && !((need == 2 && v < 0x80) || (need == 3 && v < 0x800) || (need == 4 && v < 0x10000)) && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) { cp = v; len = need; }
      }
    }
    o.push_back((char32_t)cp); i += len;
    if (o.size() >= 4096) break;
  }
  return o;
}

int registryIndexForScene(const show::ShowDocument& d, int docScene) {
  if (docScene < 0 || docScene >= (int)d.scenes.size()) return -1;
  return sceneIndexById(d.scenes[(size_t)docScene].rendererType.c_str());
}

namespace {
SceneLayer makeLayer(const show::ShowDocument& d, int docScene, const show::OutputState* os, const show::ShowSnapshot& snap, SceneClocks& clocks, Ns now) {
  SceneLayer L;
  int reg = registryIndexForScene(d, docScene);
  L.scene = reg < 0 ? 0 : reg;
  SceneParams p = defaultParams(L.scene);
  if (os) { p.brightness = os->brightness; p.speed = os->speed; p.intensity = os->intensity; p.hue = os->hue; }
  if (docScene >= 0 && docScene < (int)d.scenes.size() && reg >= 0) {
    const SceneInfo& si = sceneInfo(reg);
    for (int k = 4; k < si.nParams && k - 4 < 4; ++k) {
      auto it = d.scenes[(size_t)docScene].params.find(si.params[k].key);
      if (it != d.scenes[(size_t)docScene].params.end()) p.extra[k - 4] = it->second;
    }
  }
  L.params = p;
  L.seed = (uint32_t)(snap.seed ^ (snap.seed >> 32)) + (uint32_t)L.scene * 2654435761u + 1u;
  L.timeNs = clocks.elapsed(docScene, now);
  return L;
}
} // namespace

RenderState composeState(const ShowView& v, int outputIndex, SceneClocks& clocks, const ComposeExtras& x) {
  RenderState s;
  const show::ShowSnapshot& snap = v.snap;
  const show::ShowDocument& doc = *v.doc;
  s.elapsedNs = x.elapsedNs; s.generation = x.generation; s.features = v.features;
  s.disableStrobing = snap.strobeDisabled; s.internalScale = x.internalScale; s.shed = ShedKnobs::forLevel(x.shedLevel);
  s.refBorder = x.refBorder; s.compAspectNum = x.aspectNum; s.compAspectDen = x.aspectDen;
  const show::OutputState* os = (outputIndex >= 0 && outputIndex < (int)snap.outputs.size()) ? &snap.outputs[(size_t)outputIndex] : nullptr;
  const show::Output* oc = (outputIndex >= 0 && outputIndex < (int)doc.outputs.size()) ? &doc.outputs[(size_t)outputIndex] : nullptr;
  const bool independent = os && os->independentScene;
  int eff = (os && os->sceneIndex >= 0) ? os->sceneIndex : snap.sceneIndex;
  bool black = x.forceBlackout || snap.visualBlackedOut;
  if (snap.idleActive) {                                 // idle screen policy (REQ-D09)
    const std::string& scr = snap.idleScreen;
    if (scr == "color_field") { for (size_t i = 0; i < doc.scenes.size(); ++i) if (doc.scenes[i].rendererType == "color_field") { eff = (int)i; break; } if (eff >= 0 && doc.scenes[(size_t)eff].rendererType != "color_field") black = true; }
    else if (scr == "scene") { int f = -1; for (size_t i = 0; i < doc.scenes.size(); ++i) if (doc.scenes[i].id == doc.idle.sceneId) { f = (int)i; break; } if (f >= 0) eff = f; else black = true; }
    else black = true;                                  // "black" and "logo" (logo = branding overlay on black)
  }
  if (registryIndexForScene(doc, eff) < 0) black = true; // nothing renderable -> black, never garbage
  s.blackout = black;
  std::set<int> vis;
  int outgoing = (!independent && !snap.idleActive) ? snap.outgoingSceneIndex : -1;
  if (outgoing >= 0 && snap.transitionProgress < 1.0f && registryIndexForScene(doc, outgoing) >= 0) {
    const std::string& tt = snap.transitionType;
    float p = snap.transitionProgress;
    s.a = makeLayer(doc, outgoing, os, snap, clocks, x.elapsedNs); vis.insert(outgoing);
    if (eff >= 0) { s.b = makeLayer(doc, eff, os, snap, clocks, x.elapsedNs); vis.insert(eff); }
    if (tt == "cut") { s.transition = TransitionType::Cut; s.transitionProgress = 1.0f; }
    else if (tt == "dip_black") {                        // dip: fade A to black, then B up from black
      if (p < 0.5f) { s.transition = TransitionType::None; s.masterBrightness = 1.0f - 2.0f * p; }
      else { s.transition = TransitionType::Cut; s.masterBrightness = 2.0f * p - 1.0f; }
    } else { s.transition = TransitionType::Crossfade; s.transitionProgress = p; }   // fade / wipe (wipe renders as fade)
  } else {
    s.a = makeLayer(doc, eff, os, snap, clocks, x.elapsedNs); if (eff >= 0) vis.insert(eff);
    s.transition = TransitionType::None;
  }
  clocks.keepOnly(vis);
  const show::OverlayPolicy ov = oc ? oc->overlay : show::OverlayPolicy();
  if (x.allowOverlays && oc) {
    if (ov.text && os && !os->publishedText.empty()) { TextOverlay t; t.text = utf8ToU32(os->publishedText); s.texts.push_back(std::move(t)); }
    s.overlays.showTempo = ov.tempo && snap.tempo.valid;
    s.overlays.showKey = ov.key && snap.key.valid;
    if (ov.branding && x.branding) s.overlays.brandingImage = x.branding;
  }
  if (x.media) { s.media.frame = x.media; s.media.fit = FitMode::Fit; s.media.opacity = 1.0f; }
  if (x.cameraRouted && x.camera) { s.camera.frame = x.camera; s.camera.routed = true; s.camera.t = x.cameraTreatments; }
  return s;
}

namespace {
struct KeyW {
  std::string k;
  template <class T> void p(const T& v) { k.append(reinterpret_cast<const char*>(&v), sizeof(T)); }
  void layer(const SceneLayer& L) { p(L.scene); p(L.params.brightness); p(L.params.speed); p(L.params.intensity); p(L.params.hue); for (float e : L.params.extra) p(e); p(L.seed); p(L.timeNs); }
};
}
std::string compositionKey(const RenderState& s, int outW, int outH) {
  KeyW w;
  w.p(outW); w.p(outH); w.p(s.elapsedNs); w.p(s.generation); w.p(s.background); w.layer(s.a); w.layer(s.b);
  w.p((int)s.transition); w.p(s.transitionProgress); w.p((const void*)s.media.frame); w.p((int)s.media.fit); w.p(s.media.opacity);
  w.p((const void*)s.camera.frame); w.p(s.camera.routed);
  const CameraTreatments& t = s.camera.t;
  w.p((int)t.fit); w.p(t.pip); w.p(t.mirror); w.p(t.tintAmount); w.p(t.tintRgb); w.p(t.threshold); w.p(t.thresholdCutoff); w.p(t.posterize); w.p(t.posterizeLevels);
  w.p(t.split); w.p(t.splitCameraRight); w.p(t.trailFrames);
  w.p(t.crop.x); w.p(t.crop.y); w.p(t.crop.w); w.p(t.crop.h); w.p(t.pipRect.x); w.p(t.pipRect.y); w.p(t.pipRect.w); w.p(t.pipRect.h);
  for (const auto& tx : s.texts) { w.k.append((const char*)tx.text.data(), tx.text.size() * sizeof(char32_t)); w.p(tx.alpha); w.p(tx.scrollPx); w.p(tx.sizeFrac); w.p(tx.color); w.p(tx.box.x); w.p(tx.box.y); w.p(tx.box.w); w.p(tx.box.h); w.p((int)tx.h); w.p((int)tx.v); w.p(tx.bold); w.p(tx.shadow); }
  w.p(s.overlays.showTempo); w.p(s.overlays.showKey); w.p((const void*)s.overlays.brandingImage); w.p(s.overlays.alpha);
  w.p(s.masterBrightness); w.p(s.blackout); w.p(s.disableStrobing); w.p(s.internalScale);
  w.p(s.shed.trailFramesCap); w.p(s.shed.particlePercent); w.p(s.shed.fallbackScenes); w.p(s.shed.halfResTransition);
  w.p(s.refBorder); w.p(s.compAspectNum); w.p(s.compAspectDen);
  const FeatureSnapshot& f = s.features;
  w.p(f.sequence); w.p(f.rmsDbMix); w.p(f.onset); w.p(f.onsetStrength); for (float b : f.band) w.p(b); w.p(f.tempo.valid); w.p(f.tempo.value); w.p((int)f.tempo.source); w.p(f.key.valid); w.p(f.key.value);
  return w.k;
}

void fitScaleFrame(const Frame& in, int w, int h, Frame& out) {
  out.resize(w, h); out.pts = in.pts; out.generation = in.generation;
  if (in.w <= 0 || in.h <= 0 || w <= 0 || h <= 0 || in.px.size() < (size_t)in.w * in.h) return;
  if (in.w == w && in.h == h) { out.px = in.px; return; }
  double sc = std::min((double)w / in.w, (double)h / in.h);
  int dw = std::max(1, (int)(in.w * sc)), dh = std::max(1, (int)(in.h * sc));
  int ox = (w - dw) / 2, oy = (h - dh) / 2;
  for (int y = 0; y < dh; ++y) {
    int sy = std::min(in.h - 1, (int)((int64_t)y * in.h / dh));
    const uint32_t* srow = &in.px[(size_t)sy * in.w];
    uint32_t* drow = &out.px[(size_t)(y + oy) * w + ox];
    for (int xx = 0; xx < dw; ++xx) drow[xx] = srow[std::min(in.w - 1, (int)((int64_t)xx * in.w / dw))];
  }
}

}} // namespace ss::app
