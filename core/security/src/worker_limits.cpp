#include "ss/security/worker_limits.hpp"

namespace ss::security {

WorkerLimits defaultLimits(WorkerClass c) {
    WorkerLimits l;
    switch (c) {
        case WorkerClass::MediaDecoder: break;
        case WorkerClass::Encoder:
            l.maxMemoryBytes = 1024ull << 20; l.maxWallMs = 0xFFFFFFFFu; l.maxCpuMs = 0xFFFFFFFFu;   // long-running by design
            l.maxOutputBytes = 1ull << 40; l.maxOpenFiles = 32; l.allowNetwork = false;           // network belongs to the stream adapter
            break;
        case WorkerClass::ArchiveImporter:
            l.maxMemoryBytes = 256ull << 20; l.maxWallMs = 60000; l.maxCpuMs = 30000;
            l.maxOutputBytes = 2ull << 30; l.maxOpenFiles = 8;
            break;
    }
    return l;
}

Status validate(const WorkerLimits& l) {
    if (l.maxMemoryBytes < (16ull << 20) || l.maxMemoryBytes > (64ull << 30)) return makeError(ErrorCategory::InvalidArgument, "worker memory limit out of range");
    if (l.maxWallMs == 0 || l.maxCpuMs == 0) return makeError(ErrorCategory::InvalidArgument, "worker time limit must be non-zero");
    if (l.maxProcesses == 0 || l.maxProcesses > 8) return makeError(ErrorCategory::InvalidArgument, "worker process limit out of range");
    if (l.maxOpenFiles == 0 || l.maxOpenFiles > 1024) return makeError(ErrorCategory::InvalidArgument, "worker open-file limit out of range");
    if (l.maxOutputBytes == 0) return makeError(ErrorCategory::InvalidArgument, "worker output limit must be non-zero");
    return okStatus();
}

std::vector<std::string> unenforcedLimits(const WorkerLimits& w, const EnforcementReport& g) {
    std::vector<std::string> v;
    if (!g.memory) v.push_back("memory limit not enforced");
    if (!g.wallTime) v.push_back("wall-clock limit not enforced");
    if (!g.cpuTime) v.push_back("CPU-time limit not enforced");
    if (!g.processCount && w.maxProcesses <= 1) v.push_back("child-process limit not enforced");
    if (!g.openFiles) v.push_back("open-file limit not enforced");
    if (!g.networkBlocked && !w.allowNetwork) v.push_back("network isolation not enforced");
    if (!g.privateTemp && w.privateTempDirOnly) v.push_back("private temp directory not enforced");
    if (!g.privilegeReduced && w.reducedPrivileges) v.push_back("reduced privileges not enforced");
    return v;
}

}  // namespace ss::security
