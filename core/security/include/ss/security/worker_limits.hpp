// StageSignal security: worker limits descriptor (spec section 16: "Worker runs with reduced privileges and
// explicit memory/time/process limits where supported; report limits absent"). Pure data: the app's process
// launcher applies what the OS can enforce and fills EnforcementReport; anything unenforced is reported.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"

namespace ss::security {

enum class WorkerClass : std::uint8_t { MediaDecoder, Encoder, ArchiveImporter };

struct WorkerLimits {
    std::uint64_t maxMemoryBytes = 512ull << 20;
    std::uint32_t maxWallMs = 30000;          // hard wall-clock deadline per job
    std::uint32_t maxCpuMs = 20000;
    std::uint32_t maxProcesses = 1;           // no child spawning
    std::uint64_t maxOutputBytes = 256ull << 20;
    std::uint32_t maxOpenFiles = 16;
    bool allowNetwork = false;
    bool privateTempDirOnly = true;           // writes only into a private temporary directory
    bool reducedPrivileges = true;            // drop token / sandbox where the platform supports it
};

struct EnforcementReport {                    // filled by the platform launcher
    bool memory = false, wallTime = false, cpuTime = false, processCount = false, openFiles = false;
    bool networkBlocked = false, privateTemp = false, privilegeReduced = false;
};

WorkerLimits defaultLimits(WorkerClass c);
Status validate(const WorkerLimits& l);       // rejects zero / absurd values and allowNetwork for decoders
// Human-readable list of limits requested but not enforced, for the diagnostics screen and support bundle.
std::vector<std::string> unenforcedLimits(const WorkerLimits& wanted, const EnforcementReport& got);

}  // namespace ss::security
