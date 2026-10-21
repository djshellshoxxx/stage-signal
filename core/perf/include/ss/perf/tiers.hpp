// StageSignal perf (MOD-14): tier definitions (section 6) and memory sub-budget accounting (section 7).
#pragma once
#include <cstdint>
#include <string>
#include "ss/base/error.hpp"

namespace ss::perf {

enum class Tier : std::uint8_t { Base = 0, Standard = 1, Enhanced = 2, Show = 3 };
const char* tierName(Tier t);

struct TierDef {
    Tier tier;
    const char* name;
    unsigned minLogicalCores;
    std::uint64_t minRamMiB;
    unsigned minDisplays;
    bool needsSsd, needsGpu, needsQualifiedEncoder;
    int targetFps, compositionW, compositionH;    // basic-profile target; BASE values are spec text, others noted in docs
    const char* envelope;
};
const TierDef& tierDef(Tier t);

struct HardwareInfo {
    unsigned logicalCores = 1;
    std::uint64_t installedRamBytes = 0;
    unsigned displays = 1;
    bool ssd = false, gpuSuitable = false, qualifiedEncoder = false;
};
struct TierClassification {
    Tier tier = Tier::Base;
    bool meetsBaseFloor = false;     // false => below the 800 MHz / 512 MB floor; BASE not claimed
    std::string reason;
};
// Highest tier whose minimums are all met. Says nothing about measured performance: admission still needs
// a benchmark profile. Never upgrades a machine on CPU model or clock claims (not provided here).
TierClassification classifyTier(const HardwareInfo& hw);

// ---- memory sub-budgets --------------------------------------------------------------------
enum class MemCategory : std::uint8_t { Ui = 0, Frames, Dsp, SourceCache, Transport, Runtime, Count };
const char* memCategoryName(MemCategory c);

struct MemoryBudget {
    std::uint64_t sub[static_cast<int>(MemCategory::Count)] = {};
    std::uint64_t steadyCap = 0, peakCap = 0, hostCap = 0;
    bool provisional = false;      // true for tiers whose numbers are derived, not spec text
};
// BASE = spec section 7: UI 32, frames 12, DSP 8, source cache 8, transport 4 MiB; runtime/libs gets the rest
// of the 96 MiB steady cap (32 MiB); peak 128 MiB; host-only 64 MiB. Other tiers: the same fractions of the
// tier's minimum RAM, marked provisional.
MemoryBudget memoryBudget(Tier t);

// Bounded accounting of reserved bytes per category. Not thread-safe: owned by the control thread.
class MemoryAccountant {
public:
    explicit MemoryAccountant(const MemoryBudget& b) : b_(b) {}
    // Fails with ResourceExhausted when the category sub-budget or the aggregate steady cap would be exceeded.
    Status reserve(MemCategory c, std::uint64_t bytes);
    // Short-lived growth (e.g. decoding a still): only the aggregate peak cap and a per-category 2x ceiling apply.
    Status reserveTransient(MemCategory c, std::uint64_t bytes);
    void release(MemCategory c, std::uint64_t bytes);
    std::uint64_t used(MemCategory c) const { return used_[static_cast<int>(c)]; }
    std::uint64_t total() const;
    std::uint64_t peak() const { return peak_; }
    std::uint64_t remaining(MemCategory c) const;
    const MemoryBudget& budget() const { return b_; }
private:
    MemoryBudget b_;
    std::uint64_t used_[static_cast<int>(MemCategory::Count)] = {};
    std::uint64_t peak_ = 0;
};

}  // namespace ss::perf
