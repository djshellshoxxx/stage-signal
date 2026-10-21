// StageSignal persist: portable .stagesignal bundle planning (spec section 17, REQ-S13).
// Layout: manifest.json, show.json, media/<sha256>.<ext>. The ZIP container itself is written/read by the app with
// juce::ZipFile; this module supplies (export) the member list with contents and sizes, and (import) the validated
// member allowlist, manifest checks and the document import. Secrets and physical device identifiers are never planned.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "ss/persist/show_json.hpp"
#include "ss/security/archive.hpp"

namespace ss::persist {

constexpr const char* kBundleFormatTag = "stagesignal-bundle";
constexpr std::uint32_t kBundleVersion = 1;

// ---- export ------------------------------------------------------------------------------------
// Data for the "include media?" dialog. The core never decides silently: the UI shows these and returns BundleChoices.
struct MediaInclusionPrompt {
    std::uint32_t mediaId = 0;
    std::string fileLabel;                 // sanitised base name, never a full path
    std::string format;
    std::uint64_t byteSize = 0;
    std::string sha256;
    bool flashRisk = false;
    bool defaultInclude = true;
    std::vector<std::uint32_t> usedByScenes, usedByCues;
    std::string question;                  // ready-to-show text, for example "Include 'loop.mp4' (12.4 MiB)?"
};
std::vector<MediaInclusionPrompt> mediaPrompts(const model::ShowDocument& doc);

struct BundleChoices {
    bool includeAllMedia = false;
    std::set<std::uint32_t> includeMedia;  // used when includeAllMedia is false
    ShowExtras extras;                     // preserved metadata / migration history
};

enum class MemberKind : std::uint8_t { Manifest, Show, Media };
struct BundleMember {
    std::string path;                      // "manifest.json", "show.json" or "media/<sha256>.<ext>"
    MemberKind kind = MemberKind::Media;
    std::uint64_t size = 0;
    std::string sha256;
    std::uint32_t mediaId = 0;             // first MediaRef using this content (Media)
    std::string content;                   // inline for Manifest and Show; empty for Media (app streams the library file)
};

struct BundlePlan {
    std::vector<BundleMember> members;     // manifest.json first, then show.json, then media sorted by path
    std::vector<std::uint32_t> excludedMedia;
    std::vector<std::string> warnings;     // affected scenes/cues, fields that look like secrets, flash-risk media
    std::uint64_t totalBytes = 0;
};

Result<BundlePlan> planBundle(const model::ShowDocument& doc, const BundleChoices& choices, const std::string& appVersion = std::string());

// ---- import ------------------------------------------------------------------------------------
struct BundleManifestEntry { std::string path, sha256, kind; std::uint64_t size = 0; };
struct BundleManifest {
    std::uint32_t bundleVersion = 0;
    std::string showUuid, showName;
    SchemaVersion schema;
    std::vector<BundleManifestEntry> members;   // show.json and media; manifest.json itself is not listed
    std::vector<std::uint32_t> excludedMedia;
    const BundleManifestEntry* find(const std::string& path) const;
};

bool isAllowedBundlePath(const std::string& canonicalPath);   // show.json | manifest.json | media | media/<64 hex>.<allowed ext>

// Validates the container listing BEFORE anything is extracted or allocated: archive attacks, member allowlist,
// required members. Returns the validated entries in listing order.
Result<std::vector<security::ValidatedEntry>> validateBundleListing(const std::vector<security::ArchiveEntryInfo>& listing);

Result<BundleManifest> parseManifest(const std::string& manifestJson);
// Call per extracted member with its actual size and SHA-256.
Status checkMemberAgainstManifest(const BundleManifest& m, const std::string& path, std::uint64_t actualSize, const std::string& actualSha256);
// Parses show.json and verifies that every media reference resolves to a manifest member (or is flagged as excluded).
Result<ParsedShow> importBundleShow(const std::string& showJson, const BundleManifest& manifest);
// Content-hash check for a media file about to be used.
Status verifyMediaBytes(const model::MediaRef& ref, const void* data, std::size_t size);

}  // namespace ss::persist
