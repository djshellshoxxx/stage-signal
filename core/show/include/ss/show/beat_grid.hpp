// StageSignal show engine: beat/bar grid used for quantised triggers.
//
// Bars are counted WITHOUT assuming a detected downbeat (IF-03: "no assumed bar/downbeat certainty").
// The grid has an anchor time that counts as beat 0 of a bar. The anchor is the engine's first tick
// (engine start) or the time given to markDownbeat() (the operator's last 'bar' action). When the
// tempo changes the anchor moves to the most recent beat of the old grid, so beat phase and bar
// position are continuous across tempo changes.
#pragma once
#include <cstdint>
#include "ss/base/time.hpp"

namespace ss::show {

class BeatGrid {
public:
    explicit BeatGrid(int beatsPerBar = 4) : bpb_(beatsPerBar < 1 ? 1 : beatsPerBar) {}

    bool anchored() const { return anchored_; }
    bool valid() const { return anchored_ && bpm_ > 0.0; }
    double bpm() const { return bpm_; }

    // Set the downbeat anchor (engine start or operator 'bar' action). Resets bar counting.
    void setDownbeat(Ns t) { anchor_ = t; beatsBase_ = 0; anchored_ = true; }
    // First tick: anchor only if nothing set it yet.
    void anchorIfUnset(Ns t) { if (!anchored_) setDownbeat(t); }

    // Adopt a tempo at time `now`. Returns true when the grid changed.
    bool setTempo(double bpm, Ns now, double epsilon) {
        if (!anchored_ || bpm <= 0.0) return false;
        if (bpm_ <= 0.0) { bpm_ = bpm; return true; }          // first valid tempo: keep engine-start anchor
        if (bpm > bpm_ - epsilon && bpm < bpm_ + epsilon) return false;
        const Ns period = beatNs(bpm_);
        const std::int64_t k = floorDiv(now - anchor_, period);  // last beat of the old grid at/before now
        anchor_ += k * period;
        beatsBase_ += k;
        bpm_ = bpm;
        return true;
    }

    // Absolute beat number (since the downbeat lineage) of the last beat at or before t.
    std::int64_t beatAt(Ns t) const {
        if (!valid()) return 0;
        return beatsBase_ + floorDiv(t - anchor_, beatNs(bpm_));
    }
    int beatInBar(Ns t) const { return static_cast<int>(mod(beatAt(t), bpb_)); }
    std::int64_t barAt(Ns t) const { return floorDiv(beatAt(t), bpb_); }

    // First beat/bar boundary strictly after t. Requires valid().
    Ns boundaryAfter(Ns t, bool bar) const {
        const Ns period = beatNs(bpm_);
        std::int64_t k = floorDiv(t - anchor_, period) + 1;
        if (bar) {
            const std::int64_t r = mod(beatsBase_ + k, bpb_);
            if (r != 0) k += bpb_ - r;
        }
        return anchor_ + k * period;
    }

    static std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
        std::int64_t q = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
        return q;
    }
    static std::int64_t mod(std::int64_t a, std::int64_t b) {
        std::int64_t r = a % b;
        return r < 0 ? r + b : r;
    }

private:
    int bpb_;
    bool anchored_ = false;
    double bpm_ = 0.0;
    Ns anchor_ = 0;
    std::int64_t beatsBase_ = 0;    // beats elapsed before the current anchor, from the downbeat lineage
};

}  // namespace ss::show
