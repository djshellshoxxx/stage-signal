#include "ss/persist/show_json.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_set>

#include "nlohmann/json.hpp"
#include "ss/base/actions.hpp"
#include "ss/base/utf8.hpp"
#include "ss/security/archive.hpp"
#include "ss/security/endpoint.hpp"
#include "ss/security/redact.hpp"

namespace ss::persist {
using json = nlohmann::json;
using model::Limits;
namespace {

// ---- enum tables -------------------------------------------------------------------------------
const char* const kSceneKinds[] = {"spectrumBars", "mirrorBars", "waveformRibbon", "geometricOrbit", "radialPulse", "particleField",
    "neonGrid", "tunnel", "colorField", "starburst", "plasmaLite", "lissajousScope", "image", "gif", "video"};
const char* const kTiers[] = {"any", "low", "medium", "high", "drop"};
const char* const kTransitions[] = {"cut", "crossfade", "fadeThroughBlack"};
const char* const kTimeBases[] = {"seconds", "beats"};
const char* const kRoutes[] = {"controls", "audience", "duplicate", "independent"};
const char* const kMidiTypes[] = {"note", "controlChange", "programChange"};
const char* const kEncoders[] = {"absolute", "relativeTwosComplement", "relativeBinaryOffset", "relativeSignBit"};
const char* const kFunctions[] = {"intensity", "red", "green", "blue", "white", "strobe", "pan", "tilt", "macro", "other"};
const char* const kProtocols[] = {"artnet", "sacn"};
const char* const kModes[] = {"auto", "programmed", "hybrid"};
const char* const kFormats[] = {"png", "jpg", "jpeg", "bmp", "gif", "mp4", "mkv"};

template <std::size_t N>
bool enumInRange(std::size_t v, const char* const (&)[N]) { return v < N; }

template <std::size_t N>
const char* enumName(std::size_t v, const char* const (&names)[N]) { return v < N ? names[v] : "invalid"; }

bool isAllowedFormat(const std::string& f) {
    for (const char* a : kFormats) if (f == a) return true;
    return false;
}

// ---- validation --------------------------------------------------------------------------------
Error bad(const std::string& why) { return makeError(ErrorCategory::InvalidArgument, "show document invalid: " + why); }
Error tooMany(const char* what) { return makeError(ErrorCategory::ResourceExhausted, std::string("show document exceeds limit: ") + what); }

bool finite(double v) { return std::isfinite(v) && std::fabs(v) <= 1e9; }

bool okText(const std::string& s, std::size_t maxBytes, bool allowNewline = false) {
    if (s.size() > maxBytes || !utf8::isValid(s)) return false;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 && !(allowNewline && (u == '\n' || u == '\t'))) return false;
        if (u == 0x7F) return false;
    }
    return true;
}

bool isHex(const std::string& s, std::size_t len) {
    if (s.size() != len) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool isUuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < 36; ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool isSimpleId(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) return false;
    return true;
}

std::size_t profileFootprint(const model::FixtureProfile& p) {
    std::size_t n = 0;
    for (const auto& c : p.channels) n += c.byteWidth;
    return n;
}

}  // namespace

bool isKnownActionId(std::uint32_t a) {
    static const std::uint32_t ids[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 20, 21, 22, 23, 30, 31, 32, 33, 34, 40, 41, 42, 43, 44, 50, 51, 52,
        60, 61, 70, 71, 72, 80, 81, 82, 83, 84, 90, 91, 100, 101, 102, 103, 104, 105, 106, 107, 108, 110, 111, 112, 120, 121, 122};
    return std::find(std::begin(ids), std::end(ids), a) != std::end(ids);
}

Status validateDocument(const model::ShowDocument& d) {
    using namespace model;
    if (d.schemaMajor != kSchemaMajor) return makeError(ErrorCategory::SchemaUnsupported, "document schema major not supported by this build");
    if (!d.uuid.empty() && !isUuid(d.uuid)) return bad("uuid");
    if (!okText(d.name, PersistLimits::kMaxNameBytes) || d.name.empty()) return bad("name");
    if (!okText(d.appVersion, 64)) return bad("appVersion");
    if (!enumInRange(static_cast<std::size_t>(d.mode), kModes)) return bad("mode");
    if (d.minDwellSec < 0 || d.minDwellSec > 3600) return bad("minDwellSec");
    if (d.repeatHistory < 0 || d.repeatHistory > 64) return bad("repeatHistory");
    if (d.overrides.tempoActive &&
        !(std::isfinite(d.overrides.bpm) && d.overrides.bpm >= kMinOverrideBpm && d.overrides.bpm <= kMaxOverrideBpm)) return bad("tempoOverride");
    if (d.overrides.keyActive && (d.overrides.tonic < 0 || d.overrides.tonic > 11)) return bad("keyOverride");

    if (d.scenes.size() > Limits::kMaxScenes) return tooMany("scenes");
    if (d.cues.size() > Limits::kMaxCues) return tooMany("cues");
    if (d.messages.size() > Limits::kMaxMessages) return tooMany("messages");
    if (d.midi.size() > Limits::kMaxMidiMappings) return tooMany("midi mappings");
    if (d.patches.size() > Limits::kMaxFixtures) return tooMany("fixtures");
    if (d.fixtureProfiles.size() > PersistLimits::kMaxFixtureProfiles) return tooMany("fixture profiles");
    if (d.media.size() > PersistLimits::kMaxMedia) return tooMany("media");
    if (d.outputs.size() > PersistLimits::kMaxOutputs || d.outputs.empty()) return bad("output count");
    if (d.journal.size() > PersistLimits::kMaxJournalEntries) return tooMany("journal entries");

    // media
    std::set<std::uint32_t> mediaIds;
    for (const auto& m : d.media) {
        if (m.id == 0 || !mediaIds.insert(m.id).second) return bad("media id");
        if (!m.relativePath.empty()) {
            if (security::looksLikeUrlOrUri(m.relativePath)) return bad("media path is a URL");
            if (!security::canonicalMemberPath(m.relativePath).ok()) return bad("media path unsafe");
        }
        if (!isHex(m.sha256, 64)) return bad("media sha256");
        if (!isAllowedFormat(m.format)) return bad("media format");
        if (m.width > 65535 || m.height > 65535) return bad("media dimensions");
        if (m.byteSize > Limits::kMaxDecompressedBytes) return bad("media size");
        if (!finite(m.durationSec) || m.durationSec < 0) return bad("media duration");
    }
    // scenes
    std::set<std::uint32_t> sceneIds;
    for (const auto& s : d.scenes) {
        if (!sceneIds.insert(s.id).second) return bad("scene id");
        if (!okText(s.name, PersistLimits::kMaxNameBytes)) return bad("scene name");
        if (!enumInRange(static_cast<std::size_t>(s.kind), kSceneKinds)) return bad("scene kind");
        if (!enumInRange(static_cast<std::size_t>(s.tier), kTiers)) return bad("scene tier");
        const auto& p = s.params;
        for (float v : {p.brightness, p.speed, p.intensity, p.hue, p.saturation, p.effectAmount[0], p.effectAmount[1]})
            if (!finite(v)) return bad("scene parameter");
        if (p.effectAmount[0] < 0 || p.effectAmount[0] > 1 || p.effectAmount[1] < 0 || p.effectAmount[1] > 1) return bad("effect amount");
        if (p.prismSegments < 2 || p.prismSegments > 16) return bad("prism segments");
        if (!finite(s.costEstimate) || s.costEstimate < 0 || s.costEstimate > 1000) return bad("cost estimate");
        if (s.mediaId != 0 && !mediaIds.count(s.mediaId)) return bad("scene media reference");
    }
    // messages
    std::set<std::uint32_t> textIds;
    for (const auto& m : d.messages) {
        if (!textIds.insert(m.id).second) return bad("message id");
        if (!okText(m.utf8, Limits::kMaxTextCodePoints * 4, true)) return bad("message text");
        if (utf8::countCodePoints(m.utf8) > static_cast<long>(Limits::kMaxTextCodePoints)) return bad("message text exceeds 4096 code points");
        if (!finite(m.x) || !finite(m.y) || !finite(m.size) || m.size < 0 || m.size > 1) return bad("message geometry");
        if (!finite(m.durationSec) || !finite(m.fadeSec) || m.durationSec < 0 || m.fadeSec < 0 || m.durationSec > 86400) return bad("message timing");
    }
    // cues
    std::set<std::uint32_t> cueIds;
    for (const auto& c : d.cues) {
        if (!cueIds.insert(c.id).second) return bad("cue id");
        if (!okText(c.name, PersistLimits::kMaxNameBytes)) return bad("cue name");
        if (!sceneIds.count(c.sceneId)) return bad("cue scene reference");
        if (!finite(c.duration) || c.duration < 0 || c.duration > 1e6) return bad("cue duration");
        if (!finite(c.transitionDuration) || c.transitionDuration < 0 || c.transitionDuration > 3600) return bad("cue transition duration");
        if (!enumInRange(static_cast<std::size_t>(c.timeBase), kTimeBases)) return bad("cue time base");
        if (!enumInRange(static_cast<std::size_t>(c.transition), kTransitions)) return bad("cue transition");
        if (c.mediaId != 0 && !mediaIds.count(c.mediaId)) return bad("cue media reference");
        if (c.textId != 0 && !textIds.count(c.textId)) return bad("cue text reference");
        if (c.lightingRefs.size() > PersistLimits::kMaxLightingRefs) return tooMany("lighting refs per cue");
    }
    // outputs
    std::set<OutputId> outIds;
    for (const auto& o : d.outputs) {
        if (o.id == 0 || o.id == kAllOutputs || !outIds.insert(o.id).second) return bad("output id");
        if (!okText(o.label, PersistLimits::kMaxNameBytes) || !okText(o.matchHint, PersistLimits::kMaxNameBytes)) return bad("output text");
        if (!enumInRange(static_cast<std::size_t>(o.kind), kRoutes)) return bad("output kind");
        if (o.width < 1 || o.width > 16384 || o.height < 1 || o.height > 16384) return bad("output size");
    }
    for (const auto& o : d.outputs)
        if (o.kind == RouteKind::Duplicate && (o.duplicateOf == 0 || o.duplicateOf == o.id || !outIds.count(o.duplicateOf))) return bad("duplicate output source");
    // midi
    std::set<std::uint32_t> midiIds;
    for (const auto& m : d.midi) {
        if (!midiIds.insert(m.id).second) return bad("midi id");
        if (!okText(m.deviceName, PersistLimits::kMaxNameBytes) || !okText(m.deviceIdentity, PersistLimits::kMaxNameBytes)) return bad("midi device text");
        if (!(m.channel <= 15 || m.channel == 255)) return bad("midi channel");
        if (m.number > 127 || m.valueMin > 127 || m.valueMax > 127 || m.valueMin > m.valueMax) return bad("midi value range");
        if (!enumInRange(static_cast<std::size_t>(m.type), kMidiTypes)) return bad("midi type");
        if (!enumInRange(static_cast<std::size_t>(m.encoder), kEncoders)) return bad("midi encoder");
        if (!isKnownActionId(m.actionId)) return bad("midi action id unknown");
    }
    // fixtures
    std::set<std::string> profileIds;
    for (const auto& p : d.fixtureProfiles) {
        if (!isSimpleId(p.id) || !profileIds.insert(p.id).second) return bad("fixture profile id");
        if (!okText(p.name, PersistLimits::kMaxNameBytes)) return bad("fixture profile name");
        if (p.version < 1 || p.version > 1000) return bad("fixture profile version");
        if (p.channels.empty() || p.channels.size() > PersistLimits::kMaxChannelsPerProfile) return bad("fixture channel count");
        for (const auto& c : p.channels) {
            if (!enumInRange(static_cast<std::size_t>(c.function), kFunctions)) return bad("channel function");
            if (c.byteWidth != 1 && c.byteWidth != 2) return bad("channel width");
            if (c.legalMin > c.legalMax) return bad("channel legal range");
        }
        if (profileFootprint(p) > 512) return bad("fixture footprint exceeds a universe");
    }
    std::set<std::uint32_t> patchIds;
    for (const auto& pt : d.patches) {
        if (!patchIds.insert(pt.id).second) return bad("patch id");
        if (!okText(pt.label, PersistLimits::kMaxNameBytes)) return bad("patch label");
        const model::FixtureProfile* prof = nullptr;
        for (const auto& p : d.fixtureProfiles) if (p.id == pt.profileId) prof = &p;
        if (!prof) return bad("patch profile reference");
        if (pt.startAddress < 1 || pt.startAddress > 512) return bad("patch address");
        if (pt.universe > 32767) return bad("patch universe");
        if (pt.startAddress + profileFootprint(*prof) - 1 > 512) return bad("patch exceeds universe");
    }
    // lighting / quality
    if (!enumInRange(static_cast<std::size_t>(d.lighting.protocol), kProtocols)) return bad("lighting protocol");
    if (d.lighting.rateHz < 1 || d.lighting.rateHz > 40) return bad("lighting rate");
    if (d.lighting.sacnPriority > 200) return bad("sACN priority");
    if (!d.lighting.destination.empty() && !security::validateLightingDestination(d.lighting.destination).ok()) return bad("lighting destination");
    if (d.quality.profileHash.size() > 128 || !okText(d.quality.profileHash, 128)) return bad("quality profile hash");
    // journal
    for (const auto& j : d.journal) {
        if (j.kind.empty() || j.kind.size() > 24) return bad("journal kind");
        for (char c : j.kind) if (!((c >= 'a' && c <= 'z') || c == '_')) return bad("journal kind");
        if (!okText(j.detail, PersistLimits::kMaxJournalDetailBytes)) return bad("journal detail");
    }
    return okStatus();
}

std::vector<std::string> scanForSecrets(const model::ShowDocument& d) {
    std::vector<std::string> hits;
    auto chk = [&](const std::string& s, const std::string& where) { if (security::containsSecret(s)) hits.push_back(where); };
    chk(d.name, "name");
    for (std::size_t i = 0; i < d.scenes.size(); ++i) chk(d.scenes[i].name, "scenes[" + std::to_string(i) + "].name");
    for (std::size_t i = 0; i < d.cues.size(); ++i) chk(d.cues[i].name, "cues[" + std::to_string(i) + "].name");
    for (std::size_t i = 0; i < d.messages.size(); ++i) chk(d.messages[i].utf8, "messages[" + std::to_string(i) + "].utf8");
    for (std::size_t i = 0; i < d.outputs.size(); ++i) { chk(d.outputs[i].label, "outputs[" + std::to_string(i) + "].label"); chk(d.outputs[i].matchHint, "outputs[" + std::to_string(i) + "].matchHint"); }
    for (std::size_t i = 0; i < d.midi.size(); ++i) chk(d.midi[i].deviceName, "midi[" + std::to_string(i) + "].deviceName");
    for (std::size_t i = 0; i < d.patches.size(); ++i) chk(d.patches[i].label, "patches[" + std::to_string(i) + "].label");
    for (std::size_t i = 0; i < d.journal.size(); ++i) chk(d.journal[i].detail, "journal[" + std::to_string(i) + "].detail");
    return hits;
}

// ---- writing -----------------------------------------------------------------------------------
namespace {

std::string u64s(std::uint64_t v) { return std::to_string(v); }

json writeScene(const model::SceneDef& s) {
    json j;
    j["id"] = s.id; j["name"] = s.name; j["kind"] = enumName(static_cast<std::size_t>(s.kind), kSceneKinds);
    j["params"] = {{"brightness", static_cast<double>(s.params.brightness)}, {"speed", static_cast<double>(s.params.speed)},
                   {"intensity", static_cast<double>(s.params.intensity)}, {"hue", static_cast<double>(s.params.hue)},
                   {"saturation", static_cast<double>(s.params.saturation)},
                   {"effectAmount", {static_cast<double>(s.params.effectAmount[0]), static_cast<double>(s.params.effectAmount[1])}},
                   {"prismSegments", s.params.prismSegments}};
    j["seed"] = u64s(s.seed); j["strobe"] = s.strobe; j["costEstimate"] = static_cast<double>(s.costEstimate);
    j["tier"] = enumName(static_cast<std::size_t>(s.tier), kTiers); j["inAutoPool"] = s.inAutoPool; j["mediaId"] = s.mediaId;
    return j;
}

json writeCue(const model::CueDef& c) {
    json j;
    j["id"] = c.id; j["name"] = c.name; j["sceneId"] = c.sceneId; j["duration"] = c.duration;
    j["timeBase"] = enumName(static_cast<std::size_t>(c.timeBase), kTimeBases);
    j["transition"] = enumName(static_cast<std::size_t>(c.transition), kTransitions);
    j["transitionDuration"] = c.transitionDuration; j["mediaId"] = c.mediaId; j["textId"] = c.textId;
    j["lightingRefs"] = c.lightingRefs; j["waitForTrigger"] = c.waitForTrigger;
    return j;
}

}  // namespace

Result<std::string> showToJson(const model::ShowDocument& src, const JsonWriteOptions& opt) {
    model::ShowDocument d = src;
    d.schemaMajor = model::kSchemaMajor;
    d.schemaMinor = model::kSchemaMinor;
    const bool portable = opt.mode == ExportMode::Portable;
    if (portable) {
        for (auto& m : d.midi) m.deviceIdentity.clear();               // physical identifiers stay on this machine
        d.lighting.destination.clear();                                  // local network destination
        d.quality.profileHash.clear(); d.quality.locked = false;         // hardware-derived
        for (auto& o : d.outputs) o.matchHint = security::sanitizeForLog(o.matchHint, PersistLimits::kMaxNameBytes);
    }
    for (auto& e : d.journal) e.detail = security::sanitizeForLog(e.detail, PersistLimits::kMaxJournalDetailBytes);   // no keys in journals
    if (auto st = validateDocument(d); !st.ok()) return st.error();
    if (opt.extras.migrationHistory.size() > PersistLimits::kMaxMigrationHistory) return bad("migration history too long");

    json j;
    j["format"] = kShowFormatTag;
    j["schema"] = {{"major", d.schemaMajor}, {"minor", d.schemaMinor}};
    j["uuid"] = d.uuid; j["name"] = d.name; j["appVersion"] = d.appVersion;
    j["mode"] = enumName(static_cast<std::size_t>(d.mode), kModes);
    j["seed"] = u64s(d.seed); j["minDwellSec"] = d.minDwellSec; j["repeatHistory"] = d.repeatHistory; j["strobingDisabled"] = d.strobingDisabled;
    if (d.overrides.tempoActive) j["tempoOverride"] = {{"bpm", d.overrides.bpm}, {"source", d.overrides.tempoTap ? "tap" : "manual"}};
    if (d.overrides.keyActive) j["keyOverride"] = {{"tonic", d.overrides.tonic}, {"minor", d.overrides.keyMinor}};
    j["scenes"] = json::array(); for (const auto& s : d.scenes) j["scenes"].push_back(writeScene(s));
    j["cues"] = json::array(); for (const auto& c : d.cues) j["cues"].push_back(writeCue(c));
    j["outputs"] = json::array();
    for (const auto& o : d.outputs)
        j["outputs"].push_back({{"id", o.id}, {"label", o.label}, {"kind", enumName(static_cast<std::size_t>(o.kind), kRoutes)},
            {"duplicateOf", o.duplicateOf}, {"showBpmKey", o.showBpmKey}, {"showText", o.showText}, {"showBranding", o.showBranding},
            {"width", o.width}, {"height", o.height}, {"matchHint", o.matchHint}});
    j["media"] = json::array();
    for (const auto& m : d.media)
        j["media"].push_back({{"id", m.id}, {"relativePath", m.relativePath}, {"sha256", m.sha256}, {"format", m.format}, {"width", m.width},
            {"height", m.height}, {"byteSize", m.byteSize}, {"durationSec", m.durationSec}, {"flashRisk", m.flashRisk}});
    j["messages"] = json::array();
    for (const auto& m : d.messages)
        j["messages"].push_back({{"id", m.id}, {"utf8", m.utf8}, {"x", static_cast<double>(m.x)}, {"y", static_cast<double>(m.y)},
            {"size", static_cast<double>(m.size)}, {"color", {m.color.r, m.color.g, m.color.b}}, {"durationSec", m.durationSec},
            {"fadeSec", m.fadeSec}, {"scroll", m.scroll}});
    j["midi"] = json::array();
    for (const auto& m : d.midi)
        j["midi"].push_back({{"id", m.id}, {"deviceName", m.deviceName}, {"deviceIdentity", m.deviceIdentity}, {"channel", m.channel},
            {"type", enumName(static_cast<std::size_t>(m.type), kMidiTypes)}, {"number", m.number}, {"valueMin", m.valueMin},
            {"valueMax", m.valueMax}, {"encoder", enumName(static_cast<std::size_t>(m.encoder), kEncoders)}, {"actionId", m.actionId}});
    j["fixtureProfiles"] = json::array();
    for (const auto& p : d.fixtureProfiles) {
        json ch = json::array();
        for (const auto& c : p.channels)
            ch.push_back({{"function", enumName(static_cast<std::size_t>(c.function), kFunctions)}, {"byteWidth", c.byteWidth}, {"legalMin", c.legalMin},
                {"legalMax", c.legalMax}, {"defaultValue", c.defaultValue}, {"safeValue", c.safeValue}, {"blackoutValue", c.blackoutValue}});
        j["fixtureProfiles"].push_back({{"id", p.id}, {"name", p.name}, {"version", p.version}, {"channels", ch}});
    }
    j["patches"] = json::array();
    for (const auto& p : d.patches)
        j["patches"].push_back({{"id", p.id}, {"profileId", p.profileId}, {"label", p.label}, {"universe", p.universe},
            {"startAddress", p.startAddress}, {"group", p.group}});
    j["lighting"] = {{"protocol", enumName(static_cast<std::size_t>(d.lighting.protocol), kProtocols)}, {"destination", d.lighting.destination},
        {"broadcast", d.lighting.broadcast}, {"multicast", d.lighting.multicast}, {"sacnPriority", d.lighting.sacnPriority}, {"rateHz", d.lighting.rateHz}};
    j["quality"] = {{"profileHash", d.quality.profileHash}, {"locked", d.quality.locked}};
    j["journal"] = json::array();
    for (const auto& e : d.journal)
        j["journal"].push_back({{"monoNs", e.monoNs}, {"wallUnixMs", e.wallUnixMs}, {"kind", e.kind}, {"detail", e.detail}});
    if (!opt.extras.metadataJson.empty()) {
        json meta = json::parse(opt.extras.metadataJson, nullptr, false);
        if (meta.is_discarded() || !meta.is_object()) return bad("metadata is not a JSON object");
        j["metadata"] = std::move(meta);
    }
    if (!opt.extras.migrationHistory.empty()) j["migrations"] = opt.extras.migrationHistory;
    std::string text = j.dump(-1, ' ', false, json::error_handler_t::strict);
    if (text.size() > Limits::kMaxJsonBytes) return tooMany("serialised show size");
    return text;
}

// ---- reading -----------------------------------------------------------------------------------
namespace {

struct Fail { ErrorCategory cat; std::string msg; };
[[noreturn]] void fail(const std::string& m, ErrorCategory c = ErrorCategory::InvalidArgument) { throw Fail{c, m}; }

const json* find(const json& o, const char* k) { auto it = o.find(k); return it == o.end() ? nullptr : &*it; }

std::uint64_t getU(const json& o, const char* k, std::uint64_t max, std::uint64_t def) {
    const json* v = find(o, k);
    if (!v) return def;
    if (!v->is_number_unsigned()) fail(std::string("field type: ") + k);
    const auto u = v->get<std::uint64_t>();
    if (u > max) fail(std::string("field out of range: ") + k);
    return u;
}
std::int64_t getI(const json& o, const char* k, std::int64_t lo, std::int64_t hi, std::int64_t def) {
    const json* v = find(o, k);
    if (!v) return def;
    if (!v->is_number_integer()) fail(std::string("field type: ") + k);
    if (v->is_number_unsigned() && v->get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) fail(std::string("field out of range: ") + k);
    const auto i = v->get<std::int64_t>();
    if (i < lo || i > hi) fail(std::string("field out of range: ") + k);
    return i;
}
double getD(const json& o, const char* k, double def) {
    const json* v = find(o, k);
    if (!v) return def;
    if (!v->is_number()) fail(std::string("field type: ") + k);
    return v->get<double>();
}
bool getB(const json& o, const char* k, bool def) {
    const json* v = find(o, k);
    if (!v) return def;
    if (!v->is_boolean()) fail(std::string("field type: ") + k);
    return v->get<bool>();
}
std::string getS(const json& o, const char* k, std::size_t maxBytes, const std::string& def) {
    const json* v = find(o, k);
    if (!v) return def;
    if (!v->is_string()) fail(std::string("field type: ") + k);
    const auto& s = v->get_ref<const std::string&>();
    if (s.size() > maxBytes) fail(std::string("string too long: ") + k, ErrorCategory::ResourceExhausted);
    return s;
}
std::uint64_t getU64(const json& o, const char* k, std::uint64_t def) {
    const json* v = find(o, k);
    if (!v) return def;
    if (v->is_number_unsigned()) return v->get<std::uint64_t>();
    if (!v->is_string()) fail(std::string("field type: ") + k);
    const auto& s = v->get_ref<const std::string&>();
    if (s.empty() || s.size() > 20) fail(std::string("field value: ") + k);
    std::uint64_t r = 0;
    for (char c : s) {
        if (c < '0' || c > '9') fail(std::string("field value: ") + k);
        const auto dgt = static_cast<std::uint64_t>(c - '0');
        if (r > (UINT64_MAX - dgt) / 10) fail(std::string("field value: ") + k);
        r = r * 10 + dgt;
    }
    return r;
}
template <class E, std::size_t N>
E getEnum(const json& o, const char* k, const char* const (&names)[N], E def) {
    const json* v = find(o, k);
    if (!v) return def;
    if (!v->is_string()) fail(std::string("field type: ") + k);
    const auto& s = v->get_ref<const std::string&>();
    for (std::size_t i = 0; i < N; ++i) if (s == names[i]) return static_cast<E>(i);
    fail(std::string("unknown value for ") + k + " (unknown behaviour rejected)");
}
const json* getArr(const json& o, const char* k, std::size_t maxCount, const char* what) {
    const json* v = find(o, k);
    if (!v) return nullptr;
    if (!v->is_array()) fail(std::string("field type: ") + k);
    if (v->size() > maxCount) fail(std::string("too many ") + what, ErrorCategory::ResourceExhausted);
    return v;
}
const json& needObj(const json& v, const char* what) {
    if (!v.is_object()) fail(std::string("expected object: ") + what);
    return v;
}

bool isExecutableKey(std::string k) {
    for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* deny[] = {"script", "scripts", "exec", "execute", "command", "commands", "cmd", "shell", "macro", "macros", "plugin",
        "plugins", "hook", "hooks", "eval", "expression", "code", "onload", "oncue", "url", "uri", "include", "import", "require", "program",
        "binary", "dll", "module", "modules", "run", "action_script", "javascript", "lua", "python"};
    for (const char* d : deny) if (k == d) return true;
    return false;
}

void scanExecutable(const json& j, std::size_t depth) {
    if (depth > 12) fail("metadata nested too deeply");
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (isExecutableKey(it.key())) fail("executable behaviour is not allowed in a show file");
            scanExecutable(it.value(), depth + 1);
        }
    } else if (j.is_array()) {
        for (const auto& e : j) scanExecutable(e, depth + 1);
    }
}

void checkKeys(const json& o, std::initializer_list<const char*> allowed, const char* ctx, std::vector<std::string>& warnings) {
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool known = false;
        for (const char* a : allowed) if (it.key() == a) { known = true; break; }
        if (known) continue;
        if (isExecutableKey(it.key())) fail(std::string("executable behaviour is not allowed in a show file (") + ctx + ")");
        if (warnings.size() < PersistLimits::kMaxWarnings)
            warnings.push_back(std::string("unsupported field ignored in ") + ctx + ": " + security::sanitizeForLog(it.key(), 48));
    }
}

std::size_t jsonDepth(const std::string& t) {
    std::size_t depth = 0, maxDepth = 0;
    bool inStr = false, esc = false;
    for (char c : t) {
        if (inStr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') inStr = false; continue; }
        if (c == '"') inStr = true;
        else if (c == '[' || c == '{') { if (++depth > maxDepth) maxDepth = depth; if (maxDepth > PersistLimits::kMaxJsonDepth) return maxDepth; }
        else if (c == ']' || c == '}') { if (depth) --depth; }
    }
    return maxDepth;
}

Result<json> parseChecked(const std::string& text) {
    if (text.size() > Limits::kMaxJsonBytes) return makeError(ErrorCategory::ResourceExhausted, "show file larger than 8 MiB limit");
    if (jsonDepth(text) > PersistLimits::kMaxJsonDepth) return makeError(ErrorCategory::InvalidArgument, "show file nested too deeply");
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return makeError(ErrorCategory::DecodeFailure, "show file is not valid JSON");
    return j;
}

Result<SchemaVersion> readSchema(const json& j) {
    const json* s = find(j, "schema");
    if (!s || !s->is_object()) return makeError(ErrorCategory::InvalidArgument, "missing schema version");
    SchemaVersion v;
    try {
        v.major = static_cast<std::uint32_t>(getU(*s, "major", 1000000, 0));
        v.minor = static_cast<std::uint32_t>(getU(*s, "minor", 1000000, 0));
    } catch (const Fail& f) { return makeError(f.cat, f.msg); }
    return v;
}

}  // namespace

Result<SchemaVersion> peekSchema(const std::string& text) {
    auto j = parseChecked(text);
    if (!j) return j.error();
    return readSchema(j.value());
}

Result<ParsedShow> showFromJson(const std::string& text) {
    auto parsed = parseChecked(text);
    if (!parsed) return parsed.error();
    const json& j = parsed.value();
    ParsedShow out;
    try {
        if (getS(j, "format", 64, "") != kShowFormatTag) return makeError(ErrorCategory::DecodeFailure, "not a StageSignal show file");
        auto ver = readSchema(j);
        if (!ver) return ver.error();
        if (ver.value().major > model::kSchemaMajor)
            return makeError(ErrorCategory::SchemaUnsupported, "show was written by a newer StageSignal (schema " + std::to_string(ver.value().major) + "); update the app",
                             Recoverability::OperatorAction);
        if (ver.value().major < model::kSchemaMajor)
            return makeError(ErrorCategory::SchemaUnsupported, "show uses an older schema and must be migrated first", Recoverability::OperatorAction);
        auto& w = out.warnings;
        checkKeys(j, {"format", "schema", "uuid", "name", "appVersion", "mode", "seed", "minDwellSec", "repeatHistory", "strobingDisabled", "tempoOverride", "keyOverride", "scenes", "cues",
                      "outputs", "media", "messages", "midi", "fixtureProfiles", "patches", "lighting", "quality", "journal", "metadata", "migrations"}, "document", w);
        if (ver.value().minor > model::kSchemaMinor && w.size() < PersistLimits::kMaxWarnings)
            w.push_back("file uses a newer minor schema; unknown fields are ignored and will not be written back");

        model::ShowDocument d;
        d.schemaMajor = ver.value().major; d.schemaMinor = ver.value().minor;
        d.uuid = getS(j, "uuid", 64, ""); d.name = getS(j, "name", PersistLimits::kMaxNameBytes, "Untitled show");
        d.appVersion = getS(j, "appVersion", 64, "");
        d.mode = getEnum(j, "mode", kModes, ShowMode::Auto);
        d.seed = getU64(j, "seed", 1);
        d.minDwellSec = static_cast<int>(getI(j, "minDwellSec", 0, 3600, 8));
        d.repeatHistory = static_cast<int>(getI(j, "repeatHistory", 0, 64, 3));
        d.strobingDisabled = getB(j, "strobingDisabled", false);
        if (j.contains("tempoOverride")) {
            const auto& t = j["tempoOverride"];
            if (!t.is_object() || !t.contains("bpm") || !t["bpm"].is_number() || !t.contains("source") || !t["source"].is_string())
                return bad("tempoOverride");
            const std::string src = t["source"].get<std::string>();
            if (src != "manual" && src != "tap") return bad("tempoOverride source");
            d.overrides.tempoActive = true; d.overrides.bpm = t["bpm"].get<double>(); d.overrides.tempoTap = src == "tap";
        }
        if (j.contains("keyOverride")) {
            const auto& k = j["keyOverride"];
            if (!k.is_object() || !k.contains("tonic") || !k["tonic"].is_number_integer() || !k.contains("minor") || !k["minor"].is_boolean())
                return bad("keyOverride");
            d.overrides.keyActive = true; d.overrides.tonic = k["tonic"].get<int>(); d.overrides.keyMinor = k["minor"].get<bool>();
        }

        if (auto* a = getArr(j, "scenes", Limits::kMaxScenes, "scenes")) for (const auto& e : *a) {
            const json& o = needObj(e, "scene");
            checkKeys(o, {"id", "name", "kind", "params", "seed", "strobe", "costEstimate", "tier", "inAutoPool", "mediaId"}, "scene", w);
            model::SceneDef s;
            s.id = static_cast<std::uint32_t>(getU(o, "id", UINT32_MAX, 0)); s.name = getS(o, "name", PersistLimits::kMaxNameBytes, "");
            s.kind = getEnum(o, "kind", kSceneKinds, model::SceneKind::SpectrumBars);
            if (const json* pj = find(o, "params")) {
                const json& p = needObj(*pj, "scene params");
                checkKeys(p, {"brightness", "speed", "intensity", "hue", "saturation", "effectAmount", "prismSegments"}, "scene params", w);
                s.params.brightness = static_cast<float>(getD(p, "brightness", 1.0)); s.params.speed = static_cast<float>(getD(p, "speed", 1.0));
                s.params.intensity = static_cast<float>(getD(p, "intensity", 0.6)); s.params.hue = static_cast<float>(getD(p, "hue", 0.0));
                s.params.saturation = static_cast<float>(getD(p, "saturation", 0.8));
                s.params.prismSegments = static_cast<int>(getI(p, "prismSegments", 0, 1000, 6));
                if (auto* ea = getArr(p, "effectAmount", 2, "effect amounts")) {
                    if (ea->size() != 2 || !(*ea)[0].is_number() || !(*ea)[1].is_number()) fail("effectAmount must be two numbers");
                    s.params.effectAmount[0] = (*ea)[0].get<float>(); s.params.effectAmount[1] = (*ea)[1].get<float>();
                }
            }
            s.seed = getU64(o, "seed", 1); s.strobe = getB(o, "strobe", false); s.costEstimate = static_cast<float>(getD(o, "costEstimate", 1.0));
            s.tier = getEnum(o, "tier", kTiers, model::EnergyTier::Any); s.inAutoPool = getB(o, "inAutoPool", true);
            s.mediaId = static_cast<std::uint32_t>(getU(o, "mediaId", UINT32_MAX, 0));
            d.scenes.push_back(std::move(s));
        }
        if (auto* a = getArr(j, "cues", Limits::kMaxCues, "cues")) for (const auto& e : *a) {
            const json& o = needObj(e, "cue");
            checkKeys(o, {"id", "name", "sceneId", "duration", "timeBase", "transition", "transitionDuration", "mediaId", "textId", "lightingRefs", "waitForTrigger"}, "cue", w);
            model::CueDef c;
            c.id = static_cast<std::uint32_t>(getU(o, "id", UINT32_MAX, 0)); c.name = getS(o, "name", PersistLimits::kMaxNameBytes, "");
            c.sceneId = static_cast<std::uint32_t>(getU(o, "sceneId", UINT32_MAX, 0)); c.duration = getD(o, "duration", 8.0);
            c.timeBase = getEnum(o, "timeBase", kTimeBases, model::TimeBase::Seconds);
            c.transition = getEnum(o, "transition", kTransitions, model::TransitionKind::Crossfade);
            c.transitionDuration = getD(o, "transitionDuration", 1.0);
            c.mediaId = static_cast<std::uint32_t>(getU(o, "mediaId", UINT32_MAX, 0)); c.textId = static_cast<std::uint32_t>(getU(o, "textId", UINT32_MAX, 0));
            if (auto* refs = getArr(o, "lightingRefs", PersistLimits::kMaxLightingRefs, "lighting refs"))
                for (const auto& r : *refs) { if (!r.is_number_unsigned() || r.get<std::uint64_t>() > UINT32_MAX) fail("lightingRefs entry"); c.lightingRefs.push_back(r.get<std::uint32_t>()); }
            c.waitForTrigger = getB(o, "waitForTrigger", false);
            d.cues.push_back(std::move(c));
        }
        if (auto* a = getArr(j, "outputs", PersistLimits::kMaxOutputs, "outputs")) {
            d.outputs.clear();
            for (const auto& e : *a) {
                const json& o = needObj(e, "output");
                checkKeys(o, {"id", "label", "kind", "duplicateOf", "showBpmKey", "showText", "showBranding", "width", "height", "matchHint"}, "output", w);
                model::OutputDef od;
                od.id = static_cast<std::uint32_t>(getU(o, "id", UINT32_MAX, 1)); od.label = getS(o, "label", PersistLimits::kMaxNameBytes, "Main");
                od.kind = getEnum(o, "kind", kRoutes, RouteKind::Audience); od.duplicateOf = static_cast<std::uint32_t>(getU(o, "duplicateOf", UINT32_MAX, 0));
                od.showBpmKey = getB(o, "showBpmKey", false); od.showText = getB(o, "showText", true); od.showBranding = getB(o, "showBranding", false);
                od.width = static_cast<int>(getI(o, "width", 0, 1000000, 800)); od.height = static_cast<int>(getI(o, "height", 0, 1000000, 600));
                od.matchHint = getS(o, "matchHint", PersistLimits::kMaxNameBytes, "");
                d.outputs.push_back(std::move(od));
            }
        }
        if (auto* a = getArr(j, "media", PersistLimits::kMaxMedia, "media")) for (const auto& e : *a) {
            const json& o = needObj(e, "media");
            checkKeys(o, {"id", "relativePath", "sha256", "format", "width", "height", "byteSize", "durationSec", "flashRisk"}, "media", w);
            model::MediaRef m;
            m.id = static_cast<std::uint32_t>(getU(o, "id", UINT32_MAX, 0)); m.relativePath = getS(o, "relativePath", 200, "");
            m.sha256 = getS(o, "sha256", 64, ""); m.format = getS(o, "format", 16, "");
            m.width = static_cast<std::uint32_t>(getU(o, "width", UINT32_MAX, 0)); m.height = static_cast<std::uint32_t>(getU(o, "height", UINT32_MAX, 0));
            m.byteSize = getU(o, "byteSize", UINT64_MAX, 0); m.durationSec = getD(o, "durationSec", 0); m.flashRisk = getB(o, "flashRisk", false);
            d.media.push_back(std::move(m));
        }
        if (auto* a = getArr(j, "messages", Limits::kMaxMessages, "messages")) for (const auto& e : *a) {
            const json& o = needObj(e, "message");
            checkKeys(o, {"id", "utf8", "x", "y", "size", "color", "durationSec", "fadeSec", "scroll"}, "message", w);
            model::TextMessage m;
            m.id = static_cast<std::uint32_t>(getU(o, "id", UINT32_MAX, 0)); m.utf8 = getS(o, "utf8", Limits::kMaxTextCodePoints * 4, "");
            m.x = static_cast<float>(getD(o, "x", 0.5)); m.y = static_cast<float>(getD(o, "y", 0.5)); m.size = static_cast<float>(getD(o, "size", 0.12));
            if (auto* c = getArr(o, "color", 3, "color channels")) {
                if (c->size() != 3) fail("color must have 3 channels");
                std::uint8_t rgb[3];
                for (int i = 0; i < 3; ++i) { if (!(*c)[i].is_number_unsigned() || (*c)[i].get<std::uint64_t>() > 255) fail("color channel"); rgb[i] = (*c)[i].get<std::uint8_t>(); }
                m.color = model::Rgb{rgb[0], rgb[1], rgb[2]};
            }
            m.durationSec = getD(o, "durationSec", 5.0); m.fadeSec = getD(o, "fadeSec", 0.5); m.scroll = getB(o, "scroll", false);
            d.messages.push_back(std::move(m));
        }
        if (auto* a = getArr(j, "midi", Limits::kMaxMidiMappings, "midi mappings")) for (const auto& e : *a) {
            const json& o = needObj(e, "midi mapping");
            checkKeys(o, {"id", "deviceName", "deviceIdentity", "channel", "type", "number", "valueMin", "valueMax", "encoder", "actionId"}, "midi mapping", w);
            model::MidiMapping m;
            m.id = static_cast<std::uint32_t>(getU(o, "id", UINT32_MAX, 0)); m.deviceName = getS(o, "deviceName", PersistLimits::kMaxNameBytes, "");
            m.deviceIdentity = getS(o, "deviceIdentity", PersistLimits::kMaxNameBytes, "");
            m.channel = static_cast<std::uint8_t>(getU(o, "channel", 255, 0)); m.type = getEnum(o, "type", kMidiTypes, model::MidiMsgType::Note);
            m.number = static_cast<std::uint8_t>(getU(o, "number", 255, 0)); m.valueMin = static_cast<std::uint8_t>(getU(o, "valueMin", 255, 0));
            m.valueMax = static_cast<std::uint8_t>(getU(o, "valueMax", 255, 127)); m.encoder = getEnum(o, "encoder", kEncoders, model::EncoderMode::Absolute);
            m.actionId = static_cast<std::uint32_t>(getU(o, "actionId", UINT32_MAX, 0));
            d.midi.push_back(std::move(m));
        }
        if (auto* a = getArr(j, "fixtureProfiles", PersistLimits::kMaxFixtureProfiles, "fixture profiles")) for (const auto& e : *a) {
            const json& o = needObj(e, "fixture profile");
            checkKeys(o, {"id", "name", "version", "channels"}, "fixture profile", w);
            model::FixtureProfile p;
            p.id = getS(o, "id", 64, ""); p.name = getS(o, "name", PersistLimits::kMaxNameBytes, ""); p.version = static_cast<int>(getI(o, "version", 0, 100000, 1));
            if (auto* ch = getArr(o, "channels", PersistLimits::kMaxChannelsPerProfile, "channels")) for (const auto& ce : *ch) {
                const json& co = needObj(ce, "channel");
                checkKeys(co, {"function", "byteWidth", "legalMin", "legalMax", "defaultValue", "safeValue", "blackoutValue"}, "channel", w);
                model::FixtureChannel c;
                c.function = getEnum(co, "function", kFunctions, model::ChannelFunction::Intensity);
                c.byteWidth = static_cast<std::uint8_t>(getU(co, "byteWidth", 255, 1)); c.legalMin = static_cast<std::uint8_t>(getU(co, "legalMin", 255, 0));
                c.legalMax = static_cast<std::uint8_t>(getU(co, "legalMax", 255, 255)); c.defaultValue = static_cast<std::uint8_t>(getU(co, "defaultValue", 255, 0));
                c.safeValue = static_cast<std::uint8_t>(getU(co, "safeValue", 255, 0)); c.blackoutValue = static_cast<std::uint8_t>(getU(co, "blackoutValue", 255, 0));
                p.channels.push_back(c);
            }
            d.fixtureProfiles.push_back(std::move(p));
        }
        if (auto* a = getArr(j, "patches", Limits::kMaxFixtures, "fixtures")) for (const auto& e : *a) {
            const json& o = needObj(e, "patch");
            checkKeys(o, {"id", "profileId", "label", "universe", "startAddress", "group"}, "patch", w);
            model::FixturePatch p;
            p.id = static_cast<std::uint32_t>(getU(o, "id", UINT32_MAX, 0)); p.profileId = getS(o, "profileId", 64, ""); p.label = getS(o, "label", PersistLimits::kMaxNameBytes, "");
            p.universe = static_cast<std::uint16_t>(getU(o, "universe", 65535, 1)); p.startAddress = static_cast<std::uint16_t>(getU(o, "startAddress", 65535, 1));
            p.group = static_cast<std::uint32_t>(getU(o, "group", UINT32_MAX, 0));
            d.patches.push_back(std::move(p));
        }
        if (const json* lj = find(j, "lighting")) {
            const json& o = needObj(*lj, "lighting");
            checkKeys(o, {"protocol", "destination", "broadcast", "multicast", "sacnPriority", "rateHz"}, "lighting", w);
            d.lighting.protocol = getEnum(o, "protocol", kProtocols, model::LightingProtocol::ArtNet);
            d.lighting.destination = getS(o, "destination", 64, ""); d.lighting.broadcast = getB(o, "broadcast", false);
            d.lighting.multicast = getB(o, "multicast", false); d.lighting.sacnPriority = static_cast<std::uint16_t>(getU(o, "sacnPriority", 65535, 100));
            d.lighting.rateHz = static_cast<int>(getI(o, "rateHz", 0, 1000, 25));
        }
        if (const json* qj = find(j, "quality")) {
            const json& o = needObj(*qj, "quality");
            checkKeys(o, {"profileHash", "locked"}, "quality", w);
            d.quality.profileHash = getS(o, "profileHash", 128, ""); d.quality.locked = getB(o, "locked", false);
        }
        if (auto* a = getArr(j, "journal", PersistLimits::kMaxJournalEntries, "journal entries")) for (const auto& e : *a) {
            const json& o = needObj(e, "journal entry");
            checkKeys(o, {"monoNs", "wallUnixMs", "kind", "detail"}, "journal entry", w);
            model::JournalEntry je;
            je.monoNs = getI(o, "monoNs", INT64_MIN, INT64_MAX, 0); je.wallUnixMs = getI(o, "wallUnixMs", INT64_MIN, INT64_MAX, 0);
            je.kind = getS(o, "kind", 24, ""); je.detail = getS(o, "detail", PersistLimits::kMaxJournalDetailBytes, "");
            d.journal.push_back(std::move(je));
        }
        if (const json* m = find(j, "metadata")) {
            if (!m->is_object()) fail("metadata must be an object");
            scanExecutable(*m, 0);
            std::string dumped = m->dump();
            if (dumped.size() > PersistLimits::kMaxMetadataBytes) fail("metadata too large", ErrorCategory::ResourceExhausted);
            out.extras.metadataJson = std::move(dumped);
        }
        if (auto* a = getArr(j, "migrations", PersistLimits::kMaxMigrationHistory, "migration records")) for (const auto& e : *a) {
            if (!e.is_string() || e.get_ref<const std::string&>().size() > 128) fail("migration record");
            out.extras.migrationHistory.push_back(e.get<std::string>());
        }
        if (auto st = validateDocument(d); !st.ok()) return st.error();
        out.doc = std::move(d);
    } catch (const Fail& f) {
        return makeError(f.cat, "show document invalid: " + f.msg);
    } catch (const std::exception&) {
        return makeError(ErrorCategory::DecodeFailure, "show document could not be decoded");
    }
    return out;
}

}  // namespace ss::persist
