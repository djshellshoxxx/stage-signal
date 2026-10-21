#include "ss/persist/migration.hpp"

namespace ss::persist {

void MigrationRegistry::add(MigrationStep step) { steps_.push_back(std::move(step)); }

const MigrationStep* MigrationRegistry::from(std::uint32_t major) const {
    for (const auto& s : steps_) if (s.fromMajor == major) return &s;
    return nullptr;
}

Result<MigrationPlan> planMigration(const std::string& filePath, SchemaVersion found, const MigrationRegistry& reg,
                                    IFileSystem& fs, std::uint32_t targetMajor) {
    MigrationPlan plan;
    plan.fromMajor = found.major;
    plan.toMajor = targetMajor;
    if (found.major > targetMajor)
        return makeError(ErrorCategory::SchemaUnsupported, "file is newer than this build; it will not be migrated or overwritten", Recoverability::OperatorAction);
    if (found.major == targetMajor) return plan;   // needed = false
    std::uint32_t cur = found.major;
    while (cur < targetMajor) {
        const MigrationStep* s = reg.from(cur);
        if (!s || s->toMajor <= cur || s->toMajor > targetMajor || !s->apply)
            return makeError(ErrorCategory::SchemaUnsupported, "no migration path from schema " + std::to_string(found.major), Recoverability::OperatorAction);
        plan.stepDescriptions.push_back(s->description);
        cur = s->toMajor;
    }
    plan.needed = true;
    std::string candidate;
    for (int n = 0; n < 100; ++n) {
        candidate = filePath + ".premigrate-v" + std::to_string(found.major) + (n ? "-" + std::to_string(n + 1) : std::string());
        if (!fs.exists(candidate)) break;
    }
    plan.backupPath = candidate;
    return plan;
}

Result<MigrationResult> applyMigration(IFileSystem& fs, const MigrationPlan& plan, const std::string& originalBytes,
                                       const std::string& jsonText, const MigrationRegistry& reg) {
    MigrationResult out;
    if (!plan.needed) {
        auto p = showFromJson(jsonText);
        if (!p) return p.error();
        out.parsed = std::move(p.value());
        return out;
    }
    if (plan.backupPath.empty() || fs.exists(plan.backupPath))
        return makeError(ErrorCategory::InvalidArgument, "migration backup path unavailable");
    // 1. backup first; abort without doing anything else when it cannot be made
    if (auto st = fs.writeFile(plan.backupPath, originalBytes, true); !st.ok()) return st.error();
    // 2. migrate in memory
    std::string cur = jsonText;
    std::uint32_t major = plan.fromMajor;
    while (major < plan.toMajor) {
        const MigrationStep* s = reg.from(major);
        if (!s) return makeError(ErrorCategory::SchemaUnsupported, "migration step missing");
        auto next = s->apply(cur);
        if (!next) return next.error();
        auto ver = peekSchema(next.value());
        if (!ver) return ver.error();
        if (ver.value().major != s->toMajor) return makeError(ErrorCategory::InternalFault, "migration produced unexpected schema");
        cur = std::move(next.value());
        major = s->toMajor;
    }
    auto parsed = showFromJson(cur);
    if (!parsed) return parsed.error();
    out.parsed = std::move(parsed.value());
    out.parsed.extras.migrationHistory.push_back(std::to_string(plan.fromMajor) + "->" + std::to_string(plan.toMajor));
    out.migrated = true;
    out.backupPath = plan.backupPath;
    return out;
}

}  // namespace ss::persist
