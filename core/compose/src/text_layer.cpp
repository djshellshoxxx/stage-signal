#include "ss/compose/text_layer.hpp"
#include <algorithm>
#include <cmath>
#include "ss/base/utf8.hpp"
#include "ss/compose/raster.hpp"

namespace ss::compose {

namespace {
float clamp01(float v) { return v != v ? 0.f : std::min(1.f, std::max(0.f, v)); }
double effFade(double durationSec, double fadeSec) {
    if (!(fadeSec > 0.0)) return 0.0;
    return durationSec > 0.0 ? std::min(fadeSec, durationSec / 2.0) : fadeSec;
}
Ns secToNsD(double s) { return static_cast<Ns>(std::llround(s * 1e9)); }
double scrollDuration(double d) {
    if (!(d > 0.0)) return Limits::kDefaultScrollSec;
    return std::min(Limits::kMaxScrollSec, std::max(Limits::kMinScrollSec, d));
}
}  // namespace

std::uint32_t textEnvelope256(Ns elapsed, double durationSec, double fadeSec) {
    if (elapsed < 0) return 0;
    const double fade = effFade(durationSec, fadeSec);
    const Ns fadeNs = secToNsD(fade);
    std::uint32_t w = 256;
    if (fadeNs > 0 && elapsed < fadeNs) w = static_cast<std::uint32_t>(std::min<Ns>(256, elapsed * 256 / fadeNs));
    if (durationSec > 0.0) {
        const Ns durNs = secToNsD(durationSec);
        if (elapsed >= durNs) return 0;
        const Ns remain = durNs - elapsed;
        if (fadeNs > 0 && remain < fadeNs) w = std::min<std::uint32_t>(w, static_cast<std::uint32_t>(remain * 256 / fadeNs));
    }
    return w;
}

double scrollProgress(Ns elapsed, double durationSec) {
    if (elapsed <= 0) return 0.0;
    return nsToSec(elapsed) / scrollDuration(durationSec);
}

bool TextLayer::expired(const Slot& s, Ns now) {
    const Ns el = now - s.start;
    if (s.item.scroll) return scrollProgress(el, s.item.durationSec) >= 1.0;
    return s.item.durationSec > 0.0 && el >= secToNsD(s.item.durationSec);
}

ss::Status TextLayer::publish(const TextItem& in, Ns now) {
    using ss::ErrorCategory;
    const long cp = ss::utf8::countCodePoints(in.utf8);
    if (cp < 0) return ss::makeError(ErrorCategory::InvalidArgument, "text is not well-formed UTF-8");
    if (static_cast<std::size_t>(cp) > Limits::kMaxTextCodePoints)
        return ss::makeError(ErrorCategory::InvalidArgument, "text exceeds 4096 code points");
    if (!in.bitmap || in.bitmap->empty() || !validDims(in.bitmap->width, in.bitmap->height) ||
        in.bitmap->rgba.size() != std::size_t(in.bitmap->width) * in.bitmap->height * 4)
        return ss::makeError(ErrorCategory::InvalidArgument, "text bitmap missing or out of range");
    if (!(in.durationSec >= 0.0) || in.durationSec > Limits::kMaxTextDurationSec || !(in.fadeSec >= 0.0 || in.fadeSec != in.fadeSec))
        return ss::makeError(ErrorCategory::InvalidArgument, "text duration out of range");
    if (in.durationSec > 0.0 && in.durationSec < 0.1)
        return ss::makeError(ErrorCategory::InvalidArgument, "text duration below 0.1 s");
    TextItem item = in;
    item.x = clamp01(item.x);
    item.y = clamp01(item.y);
    item.size = std::min(1.f, std::max(0.01f, item.size != item.size ? 0.12f : item.size));
    if (!(item.fadeSec >= 0.0)) item.fadeSec = 0.0;
    Slot* target = nullptr;
    for (Slot& s : slots_)
        if (s.used && s.item.id == item.id) { target = &s; break; }
    if (!target)
        for (Slot& s : slots_)
            if (!s.used || expired(s, now)) { target = &s; break; }
    if (!target) return ss::makeError(ErrorCategory::ResourceExhausted, "no free text slot", Recoverability::OperatorAction);
    target->used = true;
    target->item = std::move(item);
    target->start = now;
    return ss::okStatus();
}

void TextLayer::clear() {
    for (Slot& s : slots_) s = Slot{};
}
void TextLayer::clearId(std::uint32_t id) {
    for (Slot& s : slots_)
        if (s.used && s.item.id == id) s = Slot{};
}
void TextLayer::purgeExpired(Ns now) {
    for (Slot& s : slots_)
        if (s.used && expired(s, now)) s = Slot{};
}
std::size_t TextLayer::activeCount(Ns now) const {
    std::size_t n = 0;
    for (const Slot& s : slots_) n += (s.used && !expired(s, now)) ? 1 : 0;
    return n;
}

void TextLayer::draw(PixelBuffer& canvas, Ns now) const {
    if (canvas.empty()) return;
    const int W = int(canvas.width), H = int(canvas.height);
    for (const Slot& s : slots_) {
        if (!s.used || expired(s, now)) continue;
        const TextItem& it = s.item;
        const PixelBuffer& bm = *it.bitmap;
        const Ns el = now - s.start;
        std::int64_t dh = std::max<std::int64_t>(1, std::llround(double(it.size) * H));
        std::int64_t dw = std::max<std::int64_t>(1, (std::int64_t(bm.width) * dh + bm.height / 2) / bm.height);
        if (!it.scroll && dw > W) {  // too wide: scale down to fit the canvas
            dw = W;
            dh = std::max<std::int64_t>(1, (std::int64_t(bm.height) * dw + bm.width / 2) / bm.width);
        }
        if (dw > 8 * std::int64_t(Limits::kMaxDimension)) dw = 8 * std::int64_t(Limits::kMaxDimension);
        const std::int64_t cy = std::llround(double(it.y) * H);
        std::int64_t top = cy - dh / 2;
        top = std::max<std::int64_t>(0, std::min<std::int64_t>(top, std::max(0, H - int(dh))));
        std::int64_t left;
        std::uint32_t env;
        if (it.scroll) {
            const double pr = std::min(1.0, scrollProgress(el, it.durationSec));
            left = static_cast<std::int64_t>(std::llround(double(W) - pr * double(W + dw)));
            env = 256;
        } else {
            const std::int64_t cx = std::llround(double(it.x) * W);
            left = cx - dw / 2;
            left = std::max<std::int64_t>(0, std::min<std::int64_t>(left, std::max<std::int64_t>(0, W - dw)));
            env = textEnvelope256(el, it.durationSec, it.fadeSec);
        }
        if (env == 0) continue;
        DrawParams dp;
        dp.sampling = Sampling::Bilinear;
        dp.blendOver = true;
        dp.opacity256 = env;
        dp.maskTint = !it.colorBitmap;
        dp.tint = it.color;
        drawScaled(bm, Rect{0, 0, int(bm.width), int(bm.height)}, canvas,
                   Rect{int(left), int(top), int(dw), int(dh)}, dp);
    }
}

}  // namespace ss::compose
