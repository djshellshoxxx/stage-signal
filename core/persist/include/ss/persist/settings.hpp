// StageSignal persist: machine-local settings (spec section 17: "Physical devices and secret references belong to
// local settings"). Never exported. Secrets themselves live in an ISecretStore; this file holds only references.
#pragma once
#include <string>
#include <vector>
#include "ss/persist/fs.hpp"

namespace ss::persist {

constexpr std::uint32_t kSettingsSchema = 1;
constexpr std::uint64_t kMaxSettingsBytes = 1u << 20;

struct DeviceHint {
    std::string role;        // audioInput | audioOutput | midiIn | midiOut | display | camera | lightingNode
    std::string identity;    // stable platform identity, never an index
    std::string label;       // human-readable name
};

struct SecretReference {
    std::string label;       // for example "YouTube main"
    std::string ref;         // "secret:<label>", see security::isValidSecretRef
};

struct LocalSettings {
    std::uint32_t schema = kSettingsSchema;
    std::vector<DeviceHint> deviceHints;
    std::string recordingFolder;           // user-chosen absolute path; local only
    std::string lastShowPath;
    std::vector<SecretReference> secretRefs;
};

Status validateSettings(const LocalSettings& s);
Result<std::string> settingsToJson(const LocalSettings& s);
// Rejects any entry that carries a plain secret value (password/secret/token/key/value fields) instead of a reference.
Result<LocalSettings> settingsFromJson(const std::string& text);

Status saveSettings(IFileSystem& fs, const std::string& path, const LocalSettings& s);   // atomic
Result<LocalSettings> loadSettings(IFileSystem& fs, const std::string& path);           // missing file -> defaults

}  // namespace ss::persist
