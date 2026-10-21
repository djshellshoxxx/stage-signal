// StageSignal diag: support-bundle composer (spec section 16).
//  - The operator sees a category preview BEFORE anything is composed; unselected categories contribute nothing.
//  - Raw audio, camera frames, key material, full media and crash dumps are never accepted, by category and by a
//    name/content check, so a caller bug cannot smuggle them in.
//  - Every text item is redacted (patterns + literal secrets) and the composed result is re-scanned; if anything secret-like
//    survives, compose() fails closed.
// The composer returns member names and contents; zipping/writing is the app's job.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/security/redact.hpp"

namespace ss::diag {

enum class BundleCategory : std::uint8_t {
    AppInfo, SystemSummary, EventJournal, Logs, Capabilities, ShowStructure, SettingsRedacted, ShowReport,
    // never includable:
    RawAudio, CameraFrames, KeyMaterial, FullMedia, CrashDump,
    Count
};

bool isForbidden(BundleCategory c);
bool defaultSelected(BundleCategory c);
const char* categoryLabel(BundleCategory c);
const char* categoryDescription(BundleCategory c);

constexpr std::size_t kBundleMaxItems = 64;
constexpr std::uint64_t kBundleMaxItemBytes = 8ull << 20, kBundleMaxTotalBytes = 32ull << 20;

struct CategoryPreview {
    BundleCategory category = BundleCategory::AppInfo;
    std::string label, description;
    bool selected = false;
    std::size_t itemCount = 0;
    std::uint64_t bytes = 0;                       // after redaction
    int redactions = 0;                            // spans that will be rewritten
    std::vector<std::string> itemNames;
    std::vector<std::string> sampleLines;          // first lines of the first item, already redacted
};

struct BundleFile { std::string name, content; };

struct ComposedBundle {
    std::vector<BundleFile> files;                 // includes manifest.txt
    int totalRedactions = 0;
    std::uint64_t totalBytes = 0;
};

class SupportBundleComposer {
public:
    SupportBundleComposer();
    // Rejects forbidden categories, unsafe names, binary payloads, key material, duplicates and oversize items.
    Status addItem(BundleCategory category, const std::string& name, const std::string& textContent);
    void setSelected(BundleCategory category, bool selected);
    void setRedactor(const security::Redactor& r) { redactor_ = r; }

    std::vector<CategoryPreview> preview() const;           // one entry per allowed category that has items
    Result<ComposedBundle> compose() const;

private:
    struct Item { BundleCategory category; std::string name, content; };
    std::vector<Item> items_;
    bool selected_[static_cast<std::size_t>(BundleCategory::Count)];
    security::Redactor redactor_;
};

// Post-condition check usable by tests and by the app before it writes the archive: names of files that still contain
// secret-like text. Always empty for a bundle produced by compose().
std::vector<std::string> scanBundleForSecrets(const ComposedBundle& b, const security::Redactor& r = security::Redactor{});

}  // namespace ss::diag
