// Pure (JUCE-free) shell helpers: UI preference store, running.lock text, versioned plugin state blob.
// Owner: shell-integration. Testable headless; AppShell.cpp wires them into AppController.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/model/show_model.hpp"

namespace ss::app {

// ---- preferences (appearance, wizard, display roles, broadcast endpoint, lighting interface ...) -------------
// Plain "key=value" lines in prefs.txt next to settings.json. Keys: [a-z0-9._-]{1,64}. Values: single line, <= 512
// bytes, no control characters. Secrets never go here (they use the secret store).
class PrefsStore {
public:
    static bool validKey(const std::string& k);
    static bool validValue(const std::string& v);
    ss::Status set(const std::string& key, const std::string& value);
    std::string get(const std::string& key, const std::string& fallback = std::string()) const;
    bool has(const std::string& key) const { return map_.count(key) != 0; }
    void erase(const std::string& key) { map_.erase(key); }
    const std::map<std::string, std::string>& all() const { return map_; }
    std::string serialise() const;
    static PrefsStore parse(const std::string& text);        // tolerant: bad lines are skipped
    static constexpr std::size_t kMaxEntries = 256, kMaxFileBytes = 256 * 1024;
private:
    std::map<std::string, std::string> map_;
};

// ---- running.lock (app-shell 8) ----------------------------------------------------------------------------
struct LockInfo {
    long long pid = 0;
    long long startUnixMs = 0;
    std::string build, mode;          // mode: "standalone" | "clap"
};
std::string lockToText(const LockInfo& l);
bool lockFromText(const std::string& text, LockInfo& out);

// ---- plugin state blob (app-shell 7.3) -----------------------------------------------------------------------
// "SSST" | u16 LE format version | u32 LE payload length | UTF-8 JSON payload | SHA-256 of payload (32 bytes).
// Never contains armed/recording/live flags, secrets or absolute paths; unknown JSON keys are ignored on load.
constexpr std::uint16_t kStateBlobVersion = 1;
constexpr std::size_t kStateBlobMaxBytes = 2u * 1024u * 1024u;

struct StateBlobContent {
    ss::model::ShowDocument doc;                       // valid unless referenceOnly
    std::vector<std::pair<std::string, std::string>> prefs;   // "ui." keys only
    bool referenceOnly = false;                        // show too large: only uuid + hash + name are stored
    std::string showUuid, showHash, showName;
};
ss::Result<std::string> encodeStateBlob(const StateBlobContent& c);
ss::Result<StateBlobContent> decodeStateBlob(const void* data, std::size_t size);

}  // namespace ss::app
