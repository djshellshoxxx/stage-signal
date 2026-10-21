#include "App/RenderHost.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace ss::app {

RenderHost::RenderHost(const ss::IClock& clock, BlackoutLatch& latch, const LatestShared<ss::show::ShowSnapshot>& show,
                       const LatestShared<SceneTable>& scenes, const ss::LatestMailbox<ss::FeatureSnapshot>& features,
                       const ss::LatestMailbox<ss::render::AudioVisualInput>& visual)
    : clock_(clock), latch_(latch), show_(show), scenes_(scenes), features_(features), visual_(visual) {}

void RenderHost::setOutputs(std::vector<OutputSpec> specs) {
    std::lock_guard<std::mutex> g(stageMutex_);
    staged_ = std::move(specs);
    haveStaged_ = true;
}

void RenderHost::applyStaged() {
    std::vector<OutputSpec> specs;
    {
        std::lock_guard<std::mutex> g(stageMutex_);
        if (!haveStaged_) return;
        specs = std::move(staged_);
        haveStaged_ = false;
    }
    std::vector<std::shared_ptr<OutputRender>> next;
    for (const auto& s : specs) {
        auto o = std::make_shared<OutputRender>();
        o->spec = s;
        if (!o->comp.prepare(s.width, s.height, 1.0f, 3, s.rotationDeg).ok()) continue;   // bad size: output stays idle/absent
        o->exchange.resize(s.width, s.height, 4);
        o->fx.prepare(s.width, s.height);
        next.push_back(std::move(o));
    }
    std::lock_guard<std::mutex> g(mapMutex_);
    outputs_.swap(next);
}

void RenderHost::ensureScene(OutputRender& o, std::unique_ptr<ss::render::SceneRenderer>& r, std::uint32_t& cur,
                             std::uint32_t wantId, const SceneTable& table, PixelBuffer& frame) {
    if (cur == wantId) return;
    cur = wantId;
    r.reset();
    auto it = table.scenes.find(wantId);
    if (it == table.scenes.end()) return;
    r = ss::render::makeScene(it->second.kind);      // nullptr for Image/Gif/Video: media layer path (engine-media-record)
    if (!r) return;
    const std::uint32_t sw = std::clamp<std::uint32_t>(o.spec.width / 2, 64, 400);
    const std::uint32_t sh = std::max<std::uint32_t>(48, sw * o.spec.height / std::max<std::uint32_t>(1, o.spec.width));
    r->prepare(sw, sh, it->second.seed);
    frame = PixelBuffer(sw, sh);
}

std::size_t RenderHost::renderFrame() {
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    applyStaged();
    const Ns now = clock_.nowMonotonic();
    const auto snap = show_.get();
    const auto table = scenes_.get();
    ss::FeatureSnapshot feat;
    const bool haveFeat = features_.read(feat);
    ss::render::AudioVisualInput av;
    const bool haveAv = visual_.read(av);

    std::vector<std::shared_ptr<OutputRender>> outs;
    { std::lock_guard<std::mutex> g(mapMutex_); outs = outputs_; }

    std::size_t published = 0;
    for (auto& op : outs) {
        OutputRender& o = *op;
        ss::compose::FrameInputs in;
        in.nowNs = now;
        const ss::show::OutputState* st = (snap && table) ? snap->output(o.spec.id) : nullptr;
        const bool blackout = (snap && (snap->visualBlackout || snap->emergencyBlackout)) || latch_.anyVisual();
        o.comp.setVisualBlackout(blackout);
        in.idle = (st == nullptr || !st->hasScene);
        if (!in.idle) {
            const std::uint32_t prevA = o.idA;
            ensureScene(o, o.sceneA, o.idA, st->sceneId, *table, o.frameA);
            if (o.idA != prevA) o.sceneStartNs = now;
            const bool transitioning = st->hasFrom && st->transitionProgress < 1.0;
            if (transitioning) ensureScene(o, o.sceneB, o.idB, st->fromSceneId, *table, o.frameB);
            ss::render::SceneInput si;
            si.timeNs = now - o.sceneStartNs;
            si.dtSec = 0.0;
            si.features = haveFeat ? &feat : nullptr;
            si.audio = haveAv ? &av : nullptr;
            si.params = &st->params;
            si.seed = st->sceneSeed;
            si.strobingDisabled = snap->strobingDisabled;
            const PixelBuffer* a = nullptr;
            if (o.sceneA) {
                o.sceneA->render(si, o.frameA);
                if (st->params.effectAmount[0] > 0.f)
                    ss::render::applyEffect(model::EffectKind::PrismMirror, st->params.effectAmount[0], si, o.frameA, o.fx);
                if (st->params.effectAmount[1] > 0.f)
                    ss::render::applyEffect(model::EffectKind::BloomPulse, st->params.effectAmount[1], si, o.frameA, o.fx);
                a = &o.frameA;
            }
            if (transitioning && o.sceneB) {
                o.sceneB->render(si, o.frameB);
                // compose: a = outgoing, b = incoming, progress 0..1
                in.scene = {&o.frameB, a, st->transition, static_cast<float>(st->transitionProgress)};
            } else {
                in.scene = {a, nullptr, st->transition, 0.f};
            }
        }
        if (layers_) layers_->fillLayers(o.spec.id, now, in);
        ss::compose::ComposedFrame f = o.comp.compose(in);
        if (!f.ok()) { dropped_.fetch_add(1, std::memory_order_relaxed); continue; }
        if (auto sf = o.exchange.acquireForWrite()) {
            if (sf->pixels.width == f.pixels->width && sf->pixels.height == f.pixels->height)
                std::memcpy(sf->pixels.rgba.data(), f.pixels->rgba.data(), f.pixels->rgba.size());
            sf->info = FrameInfo{o.spec.id, 0, ++o.sequence, now, f.blackout};
            o.exchange.publish(sf);
            ++published;
        } else {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        o.comp.release(f);
    }
    rendered_.fetch_add(published, std::memory_order_relaxed);
    const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    lastMs_.store(ms, std::memory_order_relaxed);
    if (ms > worstMs_.load(std::memory_order_relaxed)) worstMs_.store(ms, std::memory_order_relaxed);
    if (lastFrameNs_ != 0 && now > lastFrameNs_) {
        const double inst = 1e9 / double(now - lastFrameNs_);
        const double old = fps_.load(std::memory_order_relaxed);
        fps_.store(old <= 0.0 ? inst : old * 0.9 + inst * 0.1, std::memory_order_relaxed);
    }
    lastFrameNs_ = now;
    return published;
}

std::shared_ptr<const SharedFrame> RenderHost::latestFrame(OutputId id) const {
    std::shared_ptr<OutputRender> o;
    {
        std::lock_guard<std::mutex> g(mapMutex_);
        for (const auto& x : outputs_) if (x->spec.id == id) { o = x; break; }
    }
    return o ? o->exchange.latest() : nullptr;
}

RenderStats RenderHost::stats() const {
    RenderStats s;
    s.framesRendered = rendered_.load(); s.framesDropped = dropped_.load();
    s.fpsMeasured = fps_.load(); s.lastFrameMs = lastMs_.load(); s.worstFrameMs = worstMs_.load();
    return s;
}

}  // namespace ss::app
