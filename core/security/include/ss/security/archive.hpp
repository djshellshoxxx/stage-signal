// StageSignal security: archive member / relative path validation (spec section 16, REQ-S14).
// The app's ZIP reader lists entry headers only, hands them to validateArchive(), and extracts nothing
// (and allocates nothing for payloads) unless the whole listing passes. Extraction then charges an
// ExtractionBudget so a header that lies about its size cannot exceed what was validated.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"

namespace ss::security {

struct ArchiveLimits {
    // Defaults mirror model::Limits (spec section 17); persist passes the model values explicitly.
    std::size_t maxEntries = 10000;
    std::uint64_t maxTotalUncompressed = 2ull << 30;
    std::uint64_t maxEntryUncompressed = 1ull << 30;
    std::uint32_t maxRatio = 100;                 // uncompressed : compressed, per entry and overall
    std::uint64_t ratioFloorBytes = 64 * 1024;    // entries smaller than this are exempt from the ratio test
    std::size_t maxPathBytes = 200;
    std::size_t maxSegmentBytes = 100;
    std::size_t maxDepth = 8;
};

struct ArchiveEntryInfo {
    std::string path;                     // raw member name as stored in the archive
    std::uint64_t uncompressedSize = 0;
    std::uint64_t compressedSize = 0;
    bool isDirectory = false;
    bool isSymlinkOrReparse = false;      // caller-reported from the container's attributes
};

struct ValidatedEntry {
    std::string path;                     // canonical, no trailing slash
    std::string foldKey;                  // ASCII case-folded path used for duplicate detection
    std::uint64_t uncompressedSize = 0;
    std::uint64_t compressedSize = 0;
    bool isDirectory = false;
};

// Canonicalises a member path. Accepts only segments of [A-Za-z0-9._-], none empty, none starting with '.',
// none ending with '.', no Windows reserved device names. Rejects absolute paths, drive letters, UNC,
// backslashes, "..", ':' (alternate data streams), control characters and over-long names.
// A single trailing '/' is allowed (directory) and removed. Detail text never echoes the input.
Result<std::string> canonicalMemberPath(const std::string& raw, const ArchiveLimits& limits = ArchiveLimits{});

// Cheap pre-check on declared totals (call before materialising the entry list).
Status checkArchiveHeader(std::uint64_t entryCount, std::uint64_t totalUncompressed, std::uint64_t totalCompressed,
                          const ArchiveLimits& limits = ArchiveLimits{});

// Validates a whole listing: count first, then per-entry path/flags/sizes/ratio, then case-fold
// duplicates and file/directory prefix conflicts.
Result<std::vector<ValidatedEntry>> validateArchive(const std::vector<ArchiveEntryInfo>& entries,
                                                    const ArchiveLimits& limits = ArchiveLimits{});

// Tracks actual bytes written during extraction against the validated declarations.
class ExtractionBudget {
public:
    ExtractionBudget(const std::vector<ValidatedEntry>& entries, const ArchiveLimits& limits);
    Status begin(std::size_t entryIndex);          // starts counting bytes for this entry
    Status charge(std::uint64_t bytes);            // fails (ResourceExhausted) past declared entry or total size
    std::uint64_t totalWritten() const { return total_; }
private:
    std::vector<std::uint64_t> declared_;
    std::uint64_t maxTotal_ = 0, total_ = 0, entryWritten_ = 0, entryDeclared_ = 0;
    bool active_ = false;
};

}  // namespace ss::security
