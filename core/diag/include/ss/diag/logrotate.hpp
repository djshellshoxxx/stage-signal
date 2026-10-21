// StageSignal diag: log rotation planner (spec section 16: "Logs rotate at 5 x 5 MiB; retention/deletion controls available").
// Pure planning: the app lists the log directory, calls plan*(), and applies the returned operations in order.
//   active file  stagesignal.log     older files  stagesignal.1.log ... stagesignal.<maxFiles-1>.log
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/time.hpp"

namespace ss::diag {

struct LogRotationConfig {
    std::string baseName = "stagesignal";
    std::string extension = ".log";
    int maxFiles = 5;                                 // including the active file
    std::uint64_t maxBytes = 5ull * 1024 * 1024;      // per file
    std::uint64_t totalBudget() const { return maxBytes * static_cast<std::uint64_t>(maxFiles); }
};

struct LogFileInfo {
    std::string name;                                 // file name only
    std::uint64_t size = 0;
    std::int64_t modifiedUnixMs = 0;
};

struct RotateOp {
    enum class Kind : std::uint8_t { Delete, Rename } kind = Kind::Delete;
    std::string from, to;                             // `to` only for Rename
};

struct RotationPlan {
    bool rotate = false;
    std::vector<RotateOp> ops;                        // apply in order
    std::string activeName;                           // file to append to afterwards (created empty when absent)
};

std::string logFileName(const LogRotationConfig& cfg, int index);              // 0 = active
int parseLogIndex(const LogRotationConfig& cfg, const std::string& name);      // -1 when not one of ours

// Rotate when appending `incomingBytes` would push the active file past maxBytes. Also removes stray files whose index
// is >= maxFiles (for example after the limit was lowered), so total disk use stays within totalBudget().
RotationPlan planRotation(const std::vector<LogFileInfo>& existing, const LogRotationConfig& cfg, std::uint64_t incomingBytes);

// Retention controls (operator-triggered or on a schedule).
std::vector<RotateOp> planRetention(const std::vector<LogFileInfo>& existing, const LogRotationConfig& cfg, std::int64_t nowWallUnixMs, int maxAgeDays);
std::vector<RotateOp> planDeleteAll(const std::vector<LogFileInfo>& existing, const LogRotationConfig& cfg);

// One log line: "<UTC ms> <LEVEL> <redacted single-line message>\n", message capped at 2000 bytes. Level in {DEBUG,INFO,WARN,ERROR}.
std::string formatLogLine(std::int64_t wallUnixMs, const std::string& level, const std::string& message);

}  // namespace ss::diag
