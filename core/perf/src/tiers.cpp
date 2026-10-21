#include "ss/perf/tiers.hpp"
#include "ss/perf/limits.hpp"

namespace ss::perf {

const char* tierName(Tier t) {
    switch (t) {
        case Tier::Base: return "BASE";
        case Tier::Standard: return "STANDARD";
        case Tier::Enhanced: return "ENHANCED";
        case Tier::Show: return "SHOW";
    }
    return "BASE";
}

const TierDef& tierDef(Tier t) {
    static const TierDef kDefs[] = {
        {Tier::Base, "BASE", 1, 512, 2, false, false, false, 25, 800, 600,
         "controls plus one 800x600 composition at 25 FPS, audio analysis, text/stills, show modes, MIDI, one DMX universe, audio-only recording"},
        {Tier::Standard, "STANDARD", 2, 2048, 2, false, false, false, 25, 800, 600,
         "camera/GIF/video and A/V recording evaluated at 800x600; no automatic streaming qualification"},
        {Tier::Enhanced, "ENHANCED", 4, 8192, 3, true, true, false, 30, 1280, 720,
         "720p30 target, one 640x480 camera, 2 compositions; record/stream need encoder qualification"},
        {Tier::Show, "SHOW", 4, 16384, 3, true, true, true, 30, 1920, 1080,
         "1080p30-60 target, independent outputs, camera/media plus local record/broadcast"},
    };
    return kDefs[static_cast<int>(t)];
}

namespace {
bool meets(const TierDef& d, const HardwareInfo& hw) {
    if (hw.logicalCores < d.minLogicalCores) return false;
    if (hw.installedRamBytes < d.minRamMiB * kMiB) return false;
    if (hw.displays < d.minDisplays) return false;
    if (d.needsSsd && !hw.ssd) return false;
    if (d.needsGpu && !hw.gpuSuitable) return false;
    if (d.needsQualifiedEncoder && !hw.qualifiedEncoder) return false;
    return true;
}
}  // namespace

TierClassification classifyTier(const HardwareInfo& hw) {
    TierClassification c;
    // BASE floor: one core and 512 MiB installed. Displays are not part of the floor check: a single display
    // machine can still run (controls + audience output share it) but cannot claim the two-display fixture.
    if (hw.logicalCores < 1 || hw.installedRamBytes < tierDef(Tier::Base).minRamMiB * kMiB) {
        c.tier = Tier::Base; c.meetsBaseFloor = false;
        c.reason = "below the BASE floor (512 MiB installed RAM)";
        return c;
    }
    c.meetsBaseFloor = true;
    c.tier = Tier::Base;
    for (int t = static_cast<int>(Tier::Show); t >= static_cast<int>(Tier::Standard); --t) {
        if (meets(tierDef(static_cast<Tier>(t)), hw)) { c.tier = static_cast<Tier>(t); break; }
    }
    c.reason = std::string("hardware meets ") + tierName(c.tier) + " minimums";
    if (c.tier == Tier::Base && hw.displays < tierDef(Tier::Base).minDisplays)
        c.reason += "; single display, the two-display BASE fixture is not met";
    return c;
}

const char* memCategoryName(MemCategory c) {
    switch (c) {
        case MemCategory::Ui: return "ui";
        case MemCategory::Frames: return "frames";
        case MemCategory::Dsp: return "dsp";
        case MemCategory::SourceCache: return "source-cache";
        case MemCategory::Transport: return "transport";
        case MemCategory::Runtime: return "runtime";
        case MemCategory::Count: break;
    }
    return "unknown";
}

MemoryBudget memoryBudget(Tier t) {
    MemoryBudget b;
    const std::uint64_t sub[] = {32 * kMiB, 12 * kMiB, 8 * kMiB, 8 * kMiB, 4 * kMiB, 32 * kMiB};
    const std::uint64_t base = tierDef(Tier::Base).minRamMiB * kMiB;
    const std::uint64_t ram = tierDef(t).minRamMiB * kMiB;
    for (int i = 0; i < static_cast<int>(MemCategory::Count); ++i)
        b.sub[i] = t == Tier::Base ? sub[i] : static_cast<std::uint64_t>(static_cast<double>(sub[i]) * static_cast<double>(ram) / static_cast<double>(base));
    if (t == Tier::Base) {
        b.steadyCap = BaseGateLimits::kSteadyBytes; b.peakCap = BaseGateLimits::kPeakBytes; b.hostCap = BaseGateLimits::kHostBytes;
        b.provisional = false;
    } else {
        const double f = static_cast<double>(ram) / static_cast<double>(base);
        b.steadyCap = static_cast<std::uint64_t>(static_cast<double>(BaseGateLimits::kSteadyBytes) * f);
        b.peakCap = static_cast<std::uint64_t>(static_cast<double>(BaseGateLimits::kPeakBytes) * f);
        b.hostCap = static_cast<std::uint64_t>(static_cast<double>(BaseGateLimits::kHostBytes) * f);
        b.provisional = true;
    }
    return b;
}

std::uint64_t MemoryAccountant::total() const {
    std::uint64_t s = 0;
    for (std::uint64_t u : used_) s += u;
    return s;
}

std::uint64_t MemoryAccountant::remaining(MemCategory c) const {
    const int i = static_cast<int>(c);
    const std::uint64_t catLeft = b_.sub[i] > used_[i] ? b_.sub[i] - used_[i] : 0;
    const std::uint64_t t = total();
    const std::uint64_t aggLeft = b_.steadyCap > t ? b_.steadyCap - t : 0;
    return catLeft < aggLeft ? catLeft : aggLeft;
}

Status MemoryAccountant::reserve(MemCategory c, std::uint64_t bytes) {
    if (c == MemCategory::Count) return Status(makeError(ErrorCategory::InvalidArgument, "unknown memory category"));
    if (bytes > remaining(c))
        return Status(makeError(ErrorCategory::ResourceExhausted,
                                std::string("memory budget '") + memCategoryName(c) + "' would be exceeded"));
    used_[static_cast<int>(c)] += bytes;
    if (total() > peak_) peak_ = total();
    return okStatus();
}

Status MemoryAccountant::reserveTransient(MemCategory c, std::uint64_t bytes) {
    if (c == MemCategory::Count) return Status(makeError(ErrorCategory::InvalidArgument, "unknown memory category"));
    const int i = static_cast<int>(c);
    if (used_[i] + bytes > 2 * b_.sub[i] || total() + bytes > b_.peakCap)
        return Status(makeError(ErrorCategory::ResourceExhausted,
                                std::string("transient memory for '") + memCategoryName(c) + "' would exceed the peak cap"));
    used_[i] += bytes;
    if (total() > peak_) peak_ = total();
    return okStatus();
}

void MemoryAccountant::release(MemCategory c, std::uint64_t bytes) {
    if (c == MemCategory::Count) return;
    std::uint64_t& u = used_[static_cast<int>(c)];
    u = bytes > u ? 0 : u - bytes;
}

}  // namespace ss::perf
