// StageSignal persist: schema migration framework (spec section 17: "Migrations create backup first and never
// silently overwrite newer-format source").
//  - A file with a NEWER schema major is never migrated or touched (SchemaUnsupported).
//  - An OLDER file is migrated step by step in memory; before any step runs, the original bytes are copied to a
//    pre-migration backup that is never overwritten. The migrated document is only written when the operator saves.
//  - No real migrations exist yet (schema major is 1); the registry is what future majors plug into.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "ss/persist/fs.hpp"
#include "ss/persist/show_json.hpp"

namespace ss::persist {

struct MigrationStep {
    std::uint32_t fromMajor = 0, toMajor = 0;
    std::string description;
    // JSON text in (schema fromMajor) -> JSON text out (schema toMajor). Pure function, no I/O.
    std::function<Result<std::string>(const std::string&)> apply;
};

class MigrationRegistry {
public:
    void add(MigrationStep step);
    const MigrationStep* from(std::uint32_t major) const;
private:
    std::vector<MigrationStep> steps_;
};

struct MigrationPlan {
    bool needed = false;
    std::uint32_t fromMajor = 0, toMajor = 0;
    std::vector<std::string> stepDescriptions;
    std::string backupPath;      // where the untouched original will be copied FIRST
};

struct MigrationResult {
    ParsedShow parsed;
    bool migrated = false;
    std::string backupPath;      // non-empty when migrated
};

// Decides what to do for a file found at `schema`. Newer -> SchemaUnsupported; missing path -> SchemaUnsupported.
Result<MigrationPlan> planMigration(const std::string& filePath, SchemaVersion found, const MigrationRegistry& reg,
                                    IFileSystem& fs, std::uint32_t targetMajor = model::kSchemaMajor);

// Backup first (flushed, never overwriting an existing backup), then migrate in memory, then validate.
// `originalBytes` is exactly what is on disk (envelope included) and is what gets backed up.
Result<MigrationResult> applyMigration(IFileSystem& fs, const MigrationPlan& plan, const std::string& originalBytes,
                                       const std::string& jsonText, const MigrationRegistry& reg);

}  // namespace ss::persist
