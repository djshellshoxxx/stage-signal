// MOD-06 post effects applied to a finished scene frame: PrismMirror and BloomPulse (DEC-13).
#pragma once
#include <cstdint>
#include <vector>
#include "ss/render/scene.hpp"

namespace ss::render {

// Caller-owned working memory so applyEffect() never allocates. Call prepare() once per frame size (outside the
// real-time path). One scratch can serve both effects. Not thread-safe: one scratch per render thread.
struct EffectScratch {
    void prepare(std::uint32_t w, std::uint32_t h);
    bool ready(std::uint32_t w, std::uint32_t h) const { return w_ == w && h_ == h && w != 0 && h != 0; }
    std::size_t bytes() const;   // approximate heap footprint, for the BASE 12 MiB frame-pool budget

    std::uint32_t w_ = 0, h_ = 0, lw_ = 0, lh_ = 0;
    std::vector<std::uint8_t> copy;                  // PrismMirror: source copy
    std::vector<std::uint16_t> ang, rad;             // PrismMirror: polar LUT (turns Q16, half-pixel*2 units)
    std::vector<std::int16_t> sinTab;                // 1024-entry Q15 sine
    std::vector<std::uint16_t> bloomA, bloomB;       // BloomPulse: low-res RGB planes
    std::vector<std::uint16_t> colIdx, colFrac, rowIdx, rowFrac;   // BloomPulse: upsample tables
    // BloomPulse attack/release state (NEW-02): the pulse strength actually applied, 0..256. Seeded from the first call and
    // re-seeded whenever input.timeNs does not advance (same instant rendered twice, or scene time restarted).
    int pulseQ8_ = 0;
    Ns pulseTimeNs_ = 0;
    bool pulseSeeded_ = false;
};

// BloomPulse follows onset/low-band with a bounded attack and release (slew per second of input.timeNs), so even an onset
// on every frame cannot swing the picture faster than ~1.3 full cycles per second (WCAG 2.3.1, NEW-02).
// amount 0..1 (clamped; 0 or EffectKind::None = untouched). Uses input.timeNs / features / params (segments,
// speed) / strobingDisabled. If the scratch is not ready for the frame size, the frame is left untouched.
void applyEffect(model::EffectKind kind, float amount, const SceneInput& in, ss::PixelBuffer& inOut, EffectScratch& scratch);

}  // namespace ss::render
