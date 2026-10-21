#include "ss/compose/camera.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "ss/compose/limits.hpp"
#include "ss/compose/raster.hpp"

namespace ss::compose {

namespace {
float clamp01(float v) { return v != v ? 0.f : std::min(1.f, std::max(0.f, v)); }

// Apply f(r,g,b) to straight (un-premultiplied) colour, re-premultiplying; opaque pixels take the fast path.
template <class F>
void mapColour(PixelBuffer& b, F f) {
    const std::size_t n = std::size_t(b.width) * b.height;
    std::uint8_t* p = b.rgba.data();
    for (std::size_t i = 0; i < n; ++i, p += 4) {
        const std::uint32_t a = p[3];
        if (a == 255) {
            std::uint8_t r = p[0], g = p[1], bl = p[2];
            f(r, g, bl);
            p[0] = r; p[1] = g; p[2] = bl;
        } else if (a != 0) {
            std::uint8_t r = sat8((p[0] * 255u + a / 2) / a), g = sat8((p[1] * 255u + a / 2) / a),
                         bl = sat8((p[2] * 255u + a / 2) / a);
            f(r, g, bl);
            p[0] = static_cast<std::uint8_t>(div255(r * a));
            p[1] = static_cast<std::uint8_t>(div255(g * a));
            p[2] = static_cast<std::uint8_t>(div255(bl * a));
        }
    }
}
}  // namespace

CameraTreatments sanitize(CameraTreatments t) {
    t.posterizeLevels = std::min(Limits::kPosterizeMax, std::max(Limits::kPosterizeMin, t.posterizeLevels));
    t.thresholdCutoff = clamp01(t.thresholdCutoff);
    t.tintAmount = clamp01(t.tintAmount);
    if (t.trailFrames != 0) t.trailFrames = std::min(Limits::kTrailMax, std::max(Limits::kTrailMin, t.trailFrames));
    return t;
}

void mirrorHorizontal(PixelBuffer& b) {
    for (std::uint32_t y = 0; y < b.height; ++y) {
        std::uint8_t* r = b.row(y);
        for (std::uint32_t x = 0; x < b.width / 2; ++x) {
            std::uint8_t* l = r + std::size_t(x) * 4;
            std::uint8_t* h = r + std::size_t(b.width - 1 - x) * 4;
            for (int c = 0; c < 4; ++c) std::swap(l[c], h[c]);
        }
    }
}

void posterize(PixelBuffer& b, int levels) {
    levels = std::min(Limits::kPosterizeMax, std::max(Limits::kPosterizeMin, levels));
    const std::uint32_t L1 = static_cast<std::uint32_t>(levels - 1);
    mapColour(b, [&](std::uint8_t& r, std::uint8_t& g, std::uint8_t& bl) {
        auto q = [&](std::uint8_t v) {
            const std::uint32_t idx = (v * L1 + 127u) / 255u;
            return static_cast<std::uint8_t>((idx * 255u + L1 / 2) / L1);
        };
        r = q(r); g = q(g); bl = q(bl);
    });
}

void threshold(PixelBuffer& b, float cutoff01) {
    const std::uint32_t cut = static_cast<std::uint32_t>(std::lround(double(clamp01(cutoff01)) * 255.0));
    mapColour(b, [&](std::uint8_t& r, std::uint8_t& g, std::uint8_t& bl) {
        const std::uint32_t y = (54u * r + 183u * g + 19u * bl + 128u) >> 8;
        const std::uint8_t v = y >= cut ? 255 : 0;
        r = g = bl = v;
    });
}

void tint(PixelBuffer& b, Rgb c, float amount01) {
    const std::uint32_t k = static_cast<std::uint32_t>(std::lround(double(clamp01(amount01)) * 255.0));
    mapColour(b, [&](std::uint8_t& r, std::uint8_t& g, std::uint8_t& bl) {
        r = static_cast<std::uint8_t>(div255(r * (255 - k) + c.r * k));
        g = static_cast<std::uint8_t>(div255(g * (255 - k) + c.g * k));
        bl = static_cast<std::uint8_t>(div255(bl * (255 - k) + c.b * k));
    });
}

ss::Status CameraLayer::prepare(std::uint32_t maxW, std::uint32_t maxH) {
    if (!validDims(maxW, maxH))
        return ss::makeError(ss::ErrorCategory::InvalidArgument, "camera buffer dimensions out of range");
    processed_.rgba.reserve(std::size_t(maxW) * maxH * 4);
    acc_.rgba.reserve(std::size_t(maxW) * maxH * 4);
    processed_.width = processed_.height = acc_.width = acc_.height = 0;
    capW_ = maxW;
    capH_ = maxH;
    hasFrame_ = lost_ = accValid_ = false;
    state_ = CameraState::NoSignal;
    return ss::okStatus();
}

void CameraLayer::configure(const CameraLayerConfig& c) {
    const CameraTreatments old = cfg_.treatments;
    cfg_ = c;
    cfg_.treatments = sanitize(c.treatments);
    cfg_.freezeMs = std::min<std::int64_t>(Limits::kMaxFreezeMs, std::max<std::int64_t>(0, c.freezeMs));
    cfg_.staleMs = std::min<std::int64_t>(2000, std::max<std::int64_t>(50, c.staleMs));
    cfg_.split.ratio = std::min(0.9f, std::max(0.1f, cfg_.split.ratio != cfg_.split.ratio ? 0.5f : cfg_.split.ratio));
    if (cfg_.treatments.trailFrames != old.trailFrames) accValid_ = false;
}

void CameraLayer::setTrailShed(bool shed) {
    if (shed && !trailShed_) accValid_ = false;
    trailShed_ = shed;
}

bool CameraLayer::pushFrame(std::uint32_t sourceId, const PixelBuffer& f, ss::Ns now) {
    if (sourceId != cfg_.sourceId || f.empty() || f.width > capW_ || f.height > capH_ ||
        std::size_t(f.width) * f.height > processed_.rgba.capacity() / 4 ||
        f.rgba.size() != std::size_t(f.width) * f.height * 4) {
        ++rejected_;
        return false;
    }
    process(f);
    hasFrame_ = true;
    lost_ = false;
    lastFrame_ = now;
    ++accepted_;
    state_ = CameraState::Live;
    return true;
}

void CameraLayer::process(const PixelBuffer& in) {
    resizeKeep(processed_, in.width, in.height);
    std::memcpy(processed_.rgba.data(), in.rgba.data(), in.rgba.size());
    const CameraTreatments& t = cfg_.treatments;
    if (t.mirror) mirrorHorizontal(processed_);
    if (t.posterizeOn) posterize(processed_, t.posterizeLevels);
    if (t.thresholdOn) threshold(processed_, t.thresholdCutoff);
    if (t.tintOn) tint(processed_, t.tintColor, t.tintAmount);
    if (t.trailFrames >= Limits::kTrailMin && !trailShed_) {
        const std::uint32_t n = static_cast<std::uint32_t>(t.trailFrames);
        if (!accValid_ || acc_.width != processed_.width || acc_.height != processed_.height) {
            resizeKeep(acc_, processed_.width, processed_.height);
            std::memcpy(acc_.rgba.data(), processed_.rgba.data(), processed_.rgba.size());
            accValid_ = true;
        } else {
            // acc = max(current, acc * (n-1)/n) per premultiplied channel; floor => trails always fade out
            const std::size_t bytes = processed_.rgba.size();
            for (std::size_t i = 0; i < bytes; ++i) {
                const std::uint32_t decayed = (acc_.rgba[i] * (n - 1)) / n;
                acc_.rgba[i] = static_cast<std::uint8_t>(std::max<std::uint32_t>(processed_.rgba[i], decayed));
            }
            std::memcpy(processed_.rgba.data(), acc_.rgba.data(), bytes);
        }
    }
}

void CameraLayer::markLost(ss::Ns now) {
    if (!lost_) {
        lost_ = true;
        lossStart_ = now;
    }
}

CameraState CameraLayer::update(ss::Ns now) {
    if (!hasFrame_) {
        // never had a picture: nothing to freeze; a lost camera goes straight to its policy
        state_ = !lost_ ? CameraState::NoSignal
                        : (cfg_.lossPolicy == LossPolicy::Placeholder ? CameraState::Placeholder : CameraState::Hidden);
        return state_;
    }
    if (!lost_ && now - lastFrame_ >= msToNs(cfg_.staleMs)) {
        lost_ = true;
        lossStart_ = lastFrame_ + msToNs(cfg_.staleMs);
    }
    if (!lost_) {
        state_ = CameraState::Live;
    } else if (now - lossStart_ < msToNs(cfg_.freezeMs)) {
        state_ = CameraState::Frozen;
    } else {
        state_ = cfg_.lossPolicy == LossPolicy::Placeholder ? CameraState::Placeholder : CameraState::Hidden;
    }
    return state_;
}

}  // namespace ss::compose
