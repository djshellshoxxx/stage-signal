#include "ss/compose/compositor.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "ss/compose/font5x7.hpp"

namespace ss::compose {

float scaleForQualityStep(int step) {
    if (step <= 1) return 1.0f;
    const int i = std::min(step, Limits::kScaleLadderSize) - 1;
    return float(Limits::kScaleLadderPct[i]) / 100.0f;
}

ss::Status Compositor::prepare(std::uint32_t outW, std::uint32_t outH, float scale, std::size_t poolFrames, int rot) {
    using ss::ErrorCategory;
    if (!validDims(outW, outH)) return ss::makeError(ErrorCategory::InvalidArgument, "output size out of range");
    if (!(scale >= Limits::kMinScale && scale <= Limits::kMaxScale))
        return ss::makeError(ErrorCategory::InvalidArgument, "internal scale out of range");
    if (rot != 0 && rot != 90 && rot != 180 && rot != 270)
        return ss::makeError(ErrorCategory::InvalidArgument, "rotation must be 0, 90, 180 or 270");
    ss::Status st = pool_.prepare(outW, outH, poolFrames);
    if (!st.ok()) return st;
    outW_ = outW;
    outH_ = outH;
    rot_ = rot;
    userScale_ = scale;
    step_ = 0;
    trailShed_ = false;
    const std::size_t cap = std::size_t(outW) * outH * 4;
    for (PixelBuffer* b : {&stage_, &scratch_, &logical_}) {
        b->rgba.clear();
        b->rgba.shrink_to_fit();
        b->rgba.reserve(cap);
    }
    layoutBuffers();
    prepared_ = true;
    blackout_.store(false);
    seq_ = dropped_ = 0;
    return ss::okStatus();
}

void Compositor::layoutBuffers() {
    const bool swap = rot_ == 90 || rot_ == 270;
    lw_ = swap ? outH_ : outW_;
    lh_ = swap ? outW_ : outH_;
    scale_ = step_ >= 2 ? std::min(userScale_, scaleForQualityStep(step_)) : userScale_;
    iw_ = std::max<std::uint32_t>(1, std::uint32_t(std::lround(double(lw_) * scale_)));
    ih_ = std::max<std::uint32_t>(1, std::uint32_t(std::lround(double(lh_) * scale_)));
    iw_ = std::min(iw_, lw_);
    ih_ = std::min(ih_, lh_);
    resizeKeep(logical_, lw_, lh_);
    resizeKeep(stage_, iw_, ih_);
    resizeKeep(scratch_, iw_, ih_);
}

void Compositor::setConfig(const CompositorConfig& c) { cfg_ = c; }

ss::Status Compositor::setRotation(int rot) {
    if (rot != 0 && rot != 90 && rot != 180 && rot != 270)
        return ss::makeError(ss::ErrorCategory::InvalidArgument, "rotation must be 0, 90, 180 or 270");
    if (!prepared_) return ss::makeError(ss::ErrorCategory::InvalidArgument, "compositor not prepared");
    rot_ = rot;
    layoutBuffers();
    return ss::okStatus();
}

ss::Status Compositor::setInternalScale(float s) {
    if (!(s >= Limits::kMinScale && s <= Limits::kMaxScale))
        return ss::makeError(ss::ErrorCategory::InvalidArgument, "internal scale out of range");
    if (!prepared_) return ss::makeError(ss::ErrorCategory::InvalidArgument, "compositor not prepared");
    userScale_ = s;
    layoutBuffers();
    return ss::okStatus();
}

void Compositor::setQualityStep(int step) {
    step_ = std::min(Limits::kScaleLadderSize, std::max(0, step));
    trailShed_ = step_ >= 1;
    if (prepared_) layoutBuffers();
}

void Compositor::drawIdle(PixelBuffer& c) const {
    const IdleConfig& id = cfg_.idle;
    fill(c, opaque(id.color));
    if (!id.showLogo) return;
    const int W = int(c.width), H = int(c.height);
    if (id.logo && !id.logo->empty()) {
        const Rect box{W / 4, H / 4, std::max(1, W / 2), std::max(1, H / 2)};
        drawFitted(*id.logo, RectF{}, c, box, FitMode::Fit, DrawParams{});
        return;
    }
    // placeholder logo: outlined box with the label centred (built-in font; works with no system fonts)
    const int bw = std::max(3, W / 2), bh = std::max(3, H / 4);
    const Rect box{(W - bw) / 2, (H - bh) / 2, bw, bh};
    const Rgb line{static_cast<std::uint8_t>(std::min(255, id.color.r + 90)), static_cast<std::uint8_t>(std::min(255, id.color.g + 90)),
                   static_cast<std::uint8_t>(std::min(255, id.color.b + 90))};
    const Px lp = opaque(line);
    const int t = std::max(1, H / 100);
    fillRect(c, Rect{box.x, box.y, box.w, t}, lp, false);
    fillRect(c, Rect{box.x, box.y + box.h - t, box.w, t}, lp, false);
    fillRect(c, Rect{box.x, box.y, t, box.h}, lp, false);
    fillRect(c, Rect{box.x + box.w - t, box.y, t, box.h}, lp, false);
    const int s = std::max(1, H / 120);
    const int tw = textWidth(id.label.data(), s);
    if (tw > 0 && tw + 2 * t < box.w && textHeight(s) + 2 * t < box.h)
        drawText(c, box.x + (box.w - tw) / 2, box.y + (box.h - textHeight(s)) / 2, id.label.data(), s, line, 255);
}

void Compositor::drawScene(PixelBuffer& c, const PixelBuffer* scene, Rect region) const {
    fill(c, opaque(cfg_.background));
    if (!scene || scene->empty()) return;
    DrawParams dp;
    dp.sampling = cfg_.sceneSampling;
    dp.blendOver = true;
    drawFitted(*scene, RectF{}, c, region, cfg_.sceneFit, dp);
}

void Compositor::drawMedia(PixelBuffer& c, const FrameInputs& in) const {
    const int n = std::min<int>(in.mediaCount, int(Limits::kMaxMediaLayers));
    for (int i = 0; i < n; ++i) {
        const MediaLayerInput& m = in.media[std::size_t(i)];
        if (!m.frame || m.frame->empty()) continue;
        const float op = m.opacity != m.opacity ? 0.f : std::min(1.f, std::max(0.f, m.opacity));
        if (op <= 0.f) continue;
        DrawParams dp;
        dp.sampling = cfg_.sceneSampling;
        dp.opacity256 = static_cast<std::uint32_t>(std::lround(double(op) * 256.0));
        drawFitted(*m.frame, sanitizeCrop(m.crop), c, toPixelRect(m.region, int(c.width), int(c.height)), m.fit, dp);
    }
}

void Compositor::drawCamera(PixelBuffer& c, const CameraLayer& cam, CameraState st, Rect r) const {
    if (r.empty()) return;
    const CameraLayerConfig& cc = cam.config();
    if (st == CameraState::Placeholder) {
        fillRect(c, r, opaque(cc.placeholderColor), true);
        const int s = std::max(1, int(c.height) / 150);
        const char* label = "NO CAMERA";
        const int tw = textWidth(label, s);
        if (tw + 2 <= r.w && textHeight(s) + 2 <= r.h) {
            const Rgb fg{static_cast<std::uint8_t>(std::min(255, cc.placeholderColor.r + 120)),
                         static_cast<std::uint8_t>(std::min(255, cc.placeholderColor.g + 120)),
                         static_cast<std::uint8_t>(std::min(255, cc.placeholderColor.b + 120))};
            drawText(c, r.x + (r.w - tw) / 2, r.y + (r.h - textHeight(s)) / 2, label, s, fg, 255);
        }
        return;
    }
    const PixelBuffer* f = cam.frame();
    if (!f) return;
    DrawParams dp;
    dp.sampling = cfg_.sceneSampling;
    drawFitted(*f, RectF{}, c, r, cc.placement == CameraPlacement::Split ? cc.split.fit : cc.fit, dp);
}

ComposedFrame Compositor::compose(const FrameInputs& in) {
    ComposedFrame out;
    if (!prepared_) return out;
    // Blackout is sampled exactly once per admitted frame.
    const bool blackout = blackout_.load(std::memory_order_acquire);
    const int slot = pool_.acquire();
    if (slot < 0) {
        ++dropped_;
        return out;
    }

    // ---- camera state (loss handling) ----------------------------------------------------------
    CameraState cs = CameraState::NoSignal;
    bool camVisible = false;
    if (in.camera && !in.idle) {
        in.camera->setTrailShed(trailShed_);
        cs = in.camera->update(in.nowNs);
        camVisible = in.camera->visible();
    }
    const CameraLayerConfig* cc = camVisible ? &in.camera->config() : nullptr;
    const bool split = cc && cc->placement == CameraPlacement::Split;

    // ---- stage at internal resolution ---------------------------------------------------------
    PixelBuffer& st = (iw_ == lw_ && ih_ == lh_) ? logical_ : stage_;
    const int IW = int(iw_), IH = int(ih_);
    Rect sceneRegion{0, 0, IW, IH}, camSplitRect{};
    if (split) {
        const int camSpan = std::max(1, std::min((cc->split.vertical ? IW : IH) - 1,
                                                 int(std::lround(double(cc->split.ratio) * (cc->split.vertical ? IW : IH)))));
        const int total = cc->split.vertical ? IW : IH;
        const int rest = std::max(1, total - camSpan);
        if (cc->split.vertical) {
            camSplitRect = cc->split.cameraFirst ? Rect{0, 0, camSpan, IH} : Rect{IW - camSpan, 0, camSpan, IH};
            sceneRegion = cc->split.cameraFirst ? Rect{camSpan, 0, rest, IH} : Rect{0, 0, rest, IH};
        } else {
            camSplitRect = cc->split.cameraFirst ? Rect{0, 0, IW, camSpan} : Rect{0, IH - camSpan, IW, camSpan};
            sceneRegion = cc->split.cameraFirst ? Rect{0, camSpan, IW, rest} : Rect{0, 0, IW, rest};
        }
    }

    if (in.idle) {
        drawIdle(st);
    } else {
        using TK = ss::model::TransitionKind;
        const SceneInput& sc = in.scene;
        const bool haveB = sc.b != nullptr;
        const bool onlyB = haveB && (sc.kind == TK::Cut ? sc.progress > 0.f : sc.progress >= 1.f);
        const bool mix = haveB && !onlyB && sc.progress > 0.f;
        drawScene(st, onlyB ? sc.b : sc.a, sceneRegion);
        if (mix) {
            drawScene(scratch_, sc.b, sceneRegion);
            applyTransitionInPlace(st, scratch_, sc.kind, sc.progress);
        }
        drawMedia(st, in);
        if (camVisible) {
            Rect r = camSplitRect;
            if (cc->placement == CameraPlacement::Full) r = Rect{0, 0, IW, IH};
            else if (cc->placement == CameraPlacement::Pip) r = toPixelRect(cc->pip, IW, IH);
            drawCamera(st, *in.camera, cs, r);
        }
    }
    if (&st != &logical_) {
        DrawParams up;
        up.sampling = cfg_.upscaleSampling;
        up.blendOver = false;
        drawScaled(st, Rect{0, 0, IW, IH}, logical_, Rect{0, 0, int(lw_), int(lh_)}, up);
    }

    // ---- full-resolution layers: published text, approved overlays ---------------------------
    if (cfg_.policy.showText && in.text) in.text->draw(logical_, in.nowNs);
    if (in.overlay && (cfg_.policy.showBpmKey || cfg_.policy.showBranding || cfg_.policy.showDiagnostics))
        drawOverlays(logical_, cfg_.policy, *in.overlay);

    // ---- master brightness, then blackout (final) ---------------------------------------------
    const float b = in.masterBrightness != in.masterBrightness ? 1.f : std::min(1.f, std::max(0.f, in.masterBrightness));
    scaleRgb256(logical_, static_cast<std::uint32_t>(std::lround(double(b) * 256.0)));
    if (blackout) fill(logical_, Px{0, 0, 0, 255});

    PixelBuffer& frame = pool_.at(slot);
    rotateInto(logical_, rot_, frame);

    out.slot = slot;
    out.pixels = &frame;
    out.blackout = blackout;
    out.sequence = ++seq_;
    return out;
}

}  // namespace ss::compose
