// StageSignal media: asset manifest model, validator and verifier (REQ-S15, spec section 13 "Asset manifest
// fields"). A checksum proves byte identity, not ownership or license validity; this module checks that the
// declared facts are PRESENT, well formed and consistent with the bytes. It never judges a license.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "ss/base/error.hpp"

namespace ss::media {

enum class Permission : std::uint8_t { Unset, Yes, No };

struct FlashMetadata {                    // strobe metadata (REQ-S12 consumers read this)
    bool declared = false;                // the author stated an answer at all
    bool containsFlashOrStrobe = false;
    double maxFlashesPerSecond = 0;       // required (> 0) when containsFlashOrStrobe
};

struct AssetEntry {
    std::string id, version, type;        // type: "image","gif","video","font","scene-pack",...
    std::string path;                     // relative path inside the asset root (never absolute, no "..")
    std::string creator, originalUrl, retrievalDate;       // retrievalDate: YYYY-MM-DD
    bool isOriginalWork = false;          // created for the project: originalUrl may then be empty
    std::string licenseText;              // full license text or a pointer to it; may be empty when spdx is set
    std::string spdx;                     // SPDX expression, e.g. "CC-BY-4.0" or "MIT OR Apache-2.0"
    std::string copyright, attributionText, attributionLocation;
    Permission commercialUse = Permission::Unset, modificationAllowed = Permission::Unset;
    bool modified = false;
    std::string derivativeProvenance;     // how the distributed file derives from the original; "original" allowed
    std::string originalSha256, distributedSha256;         // lower-case hex, 64 chars
    std::uint32_t width = 0, height = 0;  // images/gif/video
    double durationSec = 0;               // gif/video/audio
    std::uint64_t sizeBytes = 0;
    FlashMetadata flash;
    std::vector<std::string> permittedTiers;               // "base","standard","enhanced"
};

struct Manifest {
    std::uint32_t schema = 1;
    std::vector<AssetEntry> assets;
};

struct Finding {
    std::string assetId;                  // empty for manifest-level findings
    std::string field;
    std::string message;
};

// Parse JSON text: {"schema":1,"assets":[{...camelCase AssetEntry fields...}]}. Unknown fields are ignored.
Result<Manifest> parseManifestJson(const std::string& text);

// SPDX license expression grammar check (syntax only, no license list). NONE/NOASSERTION are rejected.
bool isValidSpdxExpression(const std::string& expr);
bool isSafeRelativePath(const std::string& p);
bool isSha256Hex(const std::string& s);

// Field-level checks for one entry / whole manifest (duplicate ids and paths included).
std::vector<Finding> validateEntry(const AssetEntry& e);
std::vector<Finding> validateManifest(const Manifest& m);

// Byte access for verification; the CLI implements it with std::filesystem, tests with an in-memory fake.
class IAssetSource {
public:
    virtual ~IAssetSource() = default;
    // Streams the file through SHA-256. Error when the file is absent or unreadable.
    virtual Result<std::string> sha256Hex(const std::string& relPath, std::uint64_t* sizeBytes) = 0;
    virtual std::vector<std::string> listFiles() = 0;      // every shipped file, relative, '/' separators
};

struct VerifyOptions {
    bool requireAllFilesListed = true;               // a shipped file with no manifest entry fails the gate
    std::set<std::string> ignorePaths;               // e.g. the manifest file itself
};
struct VerifyReport {
    std::vector<Finding> findings;
    std::size_t entriesChecked = 0;
    bool ok() const { return findings.empty(); }
};
// Full gate: validateManifest + for each entry: file present, SHA-256 == distributedSha256, size matches,
// unmodified assets have originalSha256 == distributedSha256; optionally every shipped file has an entry.
VerifyReport verifyManifest(const Manifest& m, IAssetSource& files, const VerifyOptions& opt = {});

}  // namespace ss::media
