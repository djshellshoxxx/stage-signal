#include "ss/persist/settings.hpp"

#include "nlohmann/json.hpp"
#include "ss/base/utf8.hpp"
#include "ss/persist/store.hpp"
#include "ss/security/endpoint.hpp"
#include "ss/security/secret_store.hpp"

namespace ss::persist {
using json = nlohmann::json;
namespace {

Error bad(const char* why) { return makeError(ErrorCategory::InvalidArgument, std::string("settings invalid: ") + why); }

bool okText(const std::string& s, std::size_t max) {
    if (s.size() > max || !utf8::isValid(s)) return false;
    for (char c : s) if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;
    return true;
}

bool knownRole(const std::string& r) {
    for (const char* k : {"audioInput", "audioOutput", "midiIn", "midiOut", "display", "camera", "lightingNode"}) if (r == k) return true;
    return false;
}

bool secretField(const std::string& k) {
    for (const char* f : {"password", "secret", "token", "key", "value", "streamKey", "passphrase", "apiKey"}) if (k == f) return true;
    return false;
}

}  // namespace

Status validateSettings(const LocalSettings& s) {
    if (s.schema != kSettingsSchema) return makeError(ErrorCategory::SchemaUnsupported, "settings schema not supported");
    if (s.deviceHints.size() > 64) return makeError(ErrorCategory::ResourceExhausted, "too many device hints");
    for (const auto& d : s.deviceHints) {
        if (!knownRole(d.role)) return bad("device role");
        if (!okText(d.identity, 256) || d.identity.empty() || !okText(d.label, 256)) return bad("device text");
    }
    if (!okText(s.recordingFolder, 1024) || security::looksLikeUrlOrUri(s.recordingFolder)) return bad("recording folder");
    if (!okText(s.lastShowPath, 1024) || security::looksLikeUrlOrUri(s.lastShowPath)) return bad("last show path");
    if (s.secretRefs.size() > 64) return makeError(ErrorCategory::ResourceExhausted, "too many secret references");
    for (const auto& r : s.secretRefs) {
        if (!security::isValidSecretRef(r.ref)) return bad("secret reference");
        if (!okText(r.label, 128)) return bad("secret label");
    }
    return okStatus();
}

Result<std::string> settingsToJson(const LocalSettings& s) {
    if (auto st = validateSettings(s); !st.ok()) return st.error();
    json j;
    j["format"] = "stagesignal-settings";
    j["schema"] = s.schema;
    j["recordingFolder"] = s.recordingFolder;
    j["lastShowPath"] = s.lastShowPath;
    j["deviceHints"] = json::array();
    for (const auto& d : s.deviceHints) j["deviceHints"].push_back({{"role", d.role}, {"identity", d.identity}, {"label", d.label}});
    j["secretRefs"] = json::array();
    for (const auto& r : s.secretRefs) j["secretRefs"].push_back({{"label", r.label}, {"ref", r.ref}});
    return j.dump();
}

Result<LocalSettings> settingsFromJson(const std::string& text) {
    if (text.size() > kMaxSettingsBytes) return makeError(ErrorCategory::ResourceExhausted, "settings file too large");
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("format", std::string()) != "stagesignal-settings")
        return makeError(ErrorCategory::DecodeFailure, "settings file is not valid");
    LocalSettings s;
    try {
        s.schema = j.at("schema").get<std::uint32_t>();
        if (s.schema > kSettingsSchema) return makeError(ErrorCategory::SchemaUnsupported, "settings were written by a newer version");
        s.recordingFolder = j.value("recordingFolder", std::string());
        s.lastShowPath = j.value("lastShowPath", std::string());
        if (j.contains("deviceHints")) {
            const json& a = j.at("deviceHints");
            if (!a.is_array() || a.size() > 64) return makeError(ErrorCategory::ResourceExhausted, "too many device hints");
            for (const auto& e : a) s.deviceHints.push_back({e.at("role").get<std::string>(), e.at("identity").get<std::string>(), e.value("label", std::string())});
        }
        if (j.contains("secretRefs")) {
            const json& a = j.at("secretRefs");
            if (!a.is_array() || a.size() > 64) return makeError(ErrorCategory::ResourceExhausted, "too many secret references");
            for (const auto& e : a) {
                for (auto it = e.begin(); it != e.end(); ++it)
                    if (secretField(it.key())) return makeError(ErrorCategory::InvalidArgument, "settings contain a plain secret value; only references are allowed");
                s.secretRefs.push_back({e.value("label", std::string()), e.at("ref").get<std::string>()});
            }
        }
    } catch (const std::exception&) {
        return makeError(ErrorCategory::DecodeFailure, "settings file is malformed");
    }
    if (auto st = validateSettings(s); !st.ok()) return st.error();
    return s;
}

Status saveSettings(IFileSystem& fs, const std::string& path, const LocalSettings& s) {
    auto text = settingsToJson(s);
    if (!text) return text.error();
    return atomicWriteFile(fs, path, makeEnvelope(text.value(), "save", 0));
}

Result<LocalSettings> loadSettings(IFileSystem& fs, const std::string& path) {
    if (!fs.exists(path)) return LocalSettings{};
    auto bytes = fs.readFile(path, kMaxSettingsBytes + kEnvelopeSlackBytes);
    if (!bytes) return bytes.error();
    auto body = openEnvelope(bytes.value());
    if (!body) return body.error();
    return settingsFromJson(body.value());
}

}  // namespace ss::persist
