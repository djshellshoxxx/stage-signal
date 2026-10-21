// StageSignal persist: ShowDocument <-> JSON (IF-11, spec section 17).
//  - The whole document is validated before it is handed back, so a rejected file can never partially replace live state.
//  - Limits from model::Limits are checked before arrays are materialised; JSON size and nesting are checked before parse.
//  - Newer schema major  -> SchemaUnsupported. Newer minor -> accepted, unsupported fields listed in `warnings`.
//  - Unknown optional metadata ("metadata" object) is preserved verbatim; unknown executable behaviour (script/exec/
//    plugin style keys, unknown scene kinds or action ids) is rejected.
#pragma once
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/model/show_model.hpp"

namespace ss::persist {

constexpr const char* kShowFormatTag = "stagesignal-show";

struct PersistLimits {
    static constexpr std::size_t kMaxJournalEntries = 50000, kMaxNameBytes = 256, kMaxMetadataBytes = 64 * 1024,
        kMaxJsonDepth = 64, kMaxOutputs = 16, kMaxMedia = 10000, kMaxFixtureProfiles = 256, kMaxChannelsPerProfile = 512,
        kMaxJournalDetailBytes = 512, kMaxLightingRefs = 64, kMaxWarnings = 64, kMaxMigrationHistory = 64;
};

enum class ExportMode : std::uint8_t {
    Local,      // full-fidelity save for this machine
    Portable    // strips physical device identifiers, local network destinations, hardware profile hash; redacts journal
};

// Data that rides along with a document but is not part of the pure model.
struct ShowExtras {
    std::string metadataJson;                       // compact JSON object text, "" when none (unknown optional metadata)
    std::vector<std::string> migrationHistory;      // e.g. "1->2 2026-10-20"
};

struct JsonWriteOptions {
    ExportMode mode = ExportMode::Local;
    ShowExtras extras;
};

struct ParsedShow {
    model::ShowDocument doc;
    ShowExtras extras;
    std::vector<std::string> warnings;              // unsupported-field notes, never contain values
};

struct SchemaVersion { std::uint32_t major = 0, minor = 0; };

// Full semantic validation of an in-memory document (counts, references, ranges, text limits, safe media paths).
Status validateDocument(const model::ShowDocument& doc);

// Field paths (never values) of strings that look like they contain a secret, e.g. "messages[3].utf8".
// Used by the bundle planner to warn the operator; structural exclusion of secrets is separate.
std::vector<std::string> scanForSecrets(const model::ShowDocument& doc);

bool isKnownActionId(std::uint32_t actionId);

Result<std::string> showToJson(const model::ShowDocument& doc, const JsonWriteOptions& opt = JsonWriteOptions{});
Result<ParsedShow> showFromJson(const std::string& text);

// Cheap header read: size/depth checks, parse, read "schema". Used by migration and by the overwrite guard.
Result<SchemaVersion> peekSchema(const std::string& text);

}  // namespace ss::persist
