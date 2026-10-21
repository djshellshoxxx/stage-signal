#include "ss/diag/logrotate.hpp"

#include <algorithm>
#include <map>
#include "ss/diag/text.hpp"
#include "ss/security/redact.hpp"

namespace ss::diag {

std::string logFileName(const LogRotationConfig& c, int index) {
    return c.baseName + (index <= 0 ? std::string() : "." + std::to_string(index)) + c.extension;
}

int parseLogIndex(const LogRotationConfig& c, const std::string& name) {
    if (name.size() <= c.baseName.size() + c.extension.size()) {
        return name == c.baseName + c.extension ? 0 : -1;
    }
    if (name.compare(0, c.baseName.size(), c.baseName) != 0) return -1;
    if (name.compare(name.size() - c.extension.size(), c.extension.size(), c.extension) != 0) return -1;
    const std::string mid = name.substr(c.baseName.size(), name.size() - c.baseName.size() - c.extension.size());
    if (mid.size() < 2 || mid[0] != '.' || mid.size() > 6) return -1;
    int n = 0;
    for (std::size_t i = 1; i < mid.size(); ++i) {
        if (mid[i] < '0' || mid[i] > '9') return -1;
        n = n * 10 + (mid[i] - '0');
    }
    return n >= 1 && logFileName(c, n) == name ? n : -1;
}

RotationPlan planRotation(const std::vector<LogFileInfo>& existing, const LogRotationConfig& cfg, std::uint64_t incoming) {
    RotationPlan plan;
    plan.activeName = logFileName(cfg, 0);
    const int maxFiles = std::max(cfg.maxFiles, 1);
    std::map<int, const LogFileInfo*> byIndex;
    for (const auto& f : existing) { const int i = parseLogIndex(cfg, f.name); if (i >= 0) byIndex[i] = &f; }

    for (const auto& kv : byIndex)   // strays beyond the configured count
        if (kv.first >= maxFiles) plan.ops.push_back({RotateOp::Kind::Delete, kv.second->name, std::string()});

    const auto active = byIndex.find(0);
    const std::uint64_t cur = active == byIndex.end() ? 0 : active->second->size;
    const bool needRotate = active != byIndex.end() && cur > 0 && (cur >= cfg.maxBytes || incoming > cfg.maxBytes - cur);
    if (!needRotate) { plan.rotate = false; return plan; }

    plan.rotate = true;
    if (byIndex.count(maxFiles - 1) && maxFiles - 1 >= 1) plan.ops.push_back({RotateOp::Kind::Delete, logFileName(cfg, maxFiles - 1), std::string()});
    if (maxFiles == 1) {   // single file: truncate by deleting the active file
        plan.ops.push_back({RotateOp::Kind::Delete, logFileName(cfg, 0), std::string()});
        return plan;
    }
    for (int i = maxFiles - 2; i >= 0; --i)
        if (byIndex.count(i)) plan.ops.push_back({RotateOp::Kind::Rename, logFileName(cfg, i), logFileName(cfg, i + 1)});
    return plan;
}

std::vector<RotateOp> planRetention(const std::vector<LogFileInfo>& existing, const LogRotationConfig& cfg, std::int64_t now, int maxAgeDays) {
    std::vector<RotateOp> ops;
    const std::int64_t cutoff = now - static_cast<std::int64_t>(std::max(maxAgeDays, 0)) * 86400000LL;
    for (const auto& f : existing)
        if (parseLogIndex(cfg, f.name) >= 0 && f.modifiedUnixMs < cutoff) ops.push_back({RotateOp::Kind::Delete, f.name, std::string()});
    return ops;
}

std::vector<RotateOp> planDeleteAll(const std::vector<LogFileInfo>& existing, const LogRotationConfig& cfg) {
    std::vector<RotateOp> ops;
    for (const auto& f : existing) if (parseLogIndex(cfg, f.name) >= 0) ops.push_back({RotateOp::Kind::Delete, f.name, std::string()});
    return ops;
}

std::string formatLogLine(std::int64_t wallUnixMs, const std::string& level, const std::string& message) {
    std::string lv = level;
    if (lv != "DEBUG" && lv != "INFO" && lv != "WARN" && lv != "ERROR") lv = "INFO";
    return formatUtcIso(wallUnixMs, true) + " " + lv + " " + security::sanitizeForLog(message, 2000) + "\n";
}

}  // namespace ss::diag
