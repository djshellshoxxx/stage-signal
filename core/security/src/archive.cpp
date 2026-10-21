#include "ss/security/archive.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_set>

namespace ss::security {
namespace {

Error bad(const char* why) { return makeError(ErrorCategory::InvalidArgument, std::string("archive path rejected: ") + why); }
Error exhausted(const char* why) { return makeError(ErrorCategory::ResourceExhausted, std::string("archive limit exceeded: ") + why); }

bool segChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-';
}

bool reservedDeviceName(const std::string& seg) {
    std::string stem = seg.substr(0, seg.find('.'));
    for (auto& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" || stem == "CONOUT$") return true;
    if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '0' && stem[3] <= '9') return true;
    return false;
}

std::string foldAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Result<std::string> canonicalMemberPath(const std::string& raw, const ArchiveLimits& limits) {
    if (raw.empty()) return bad("empty");
    if (raw.size() > limits.maxPathBytes) return bad("too long");
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\') return bad("backslash");
        if (u < 0x20 || u == 0x7F) return bad("control character");
        if (c == ':') return bad("colon (drive letter or stream)");
        if (u >= 0x80) return bad("non-ASCII");
    }
    if (raw[0] == '/') return bad("absolute or UNC");
    std::string p = raw;
    if (p.back() == '/') p.pop_back();
    if (p.empty()) return bad("empty");
    std::size_t depth = 0, i = 0;
    while (i <= p.size()) {
        std::size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string seg = p.substr(i, j - i);
        if (seg.empty()) return bad("empty segment");
        if (seg == "." || seg == "..") return bad("dot segment");
        if (seg[0] == '.') return bad("hidden segment");
        if (seg.back() == '.') return bad("trailing dot");
        if (seg.size() > limits.maxSegmentBytes) return bad("segment too long");
        for (char c : seg) if (!segChar(c)) return bad("character not allowed");
        if (reservedDeviceName(seg)) return bad("reserved device name");
        if (++depth > limits.maxDepth) return bad("too deep");
        i = j + 1;
    }
    return p;
}

Status checkArchiveHeader(std::uint64_t entryCount, std::uint64_t totalUncompressed, std::uint64_t totalCompressed,
                          const ArchiveLimits& limits) {
    if (entryCount > limits.maxEntries) return exhausted("entry count");
    if (totalUncompressed > limits.maxTotalUncompressed) return exhausted("total uncompressed size");
    if (totalUncompressed > limits.ratioFloorBytes) {
        if (totalCompressed == 0 || totalUncompressed / limits.maxRatio > totalCompressed) return exhausted("compression ratio");
    }
    return okStatus();
}

Result<std::vector<ValidatedEntry>> validateArchive(const std::vector<ArchiveEntryInfo>& entries, const ArchiveLimits& limits) {
    if (entries.size() > limits.maxEntries) return exhausted("entry count");   // before any per-entry work
    std::vector<ValidatedEntry> out;
    out.reserve(entries.size());
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> fileKeys;
    std::uint64_t totalU = 0, totalC = 0;
    for (const auto& e : entries) {
        if (e.isSymlinkOrReparse) return bad("symlink or reparse point");
        auto canon = canonicalMemberPath(e.path, limits);
        if (!canon) return canon.error();
        const bool slashed = !e.path.empty() && e.path.back() == '/';
        if (slashed != e.isDirectory) return bad("directory flag mismatch");
        ValidatedEntry v;
        v.path = canon.value();
        v.foldKey = foldAscii(v.path);
        v.uncompressedSize = e.uncompressedSize;
        v.compressedSize = e.compressedSize;
        v.isDirectory = e.isDirectory;
        if (!seen.insert(v.foldKey).second) return bad("duplicate canonical path");
        if (!v.isDirectory) {
            fileKeys.insert(v.foldKey);
            if (e.uncompressedSize > limits.maxEntryUncompressed) return exhausted("entry size");
            if (e.uncompressedSize > limits.ratioFloorBytes &&
                (e.compressedSize == 0 || e.uncompressedSize / limits.maxRatio > e.compressedSize))
                return exhausted("entry compression ratio");
            if (e.uncompressedSize > limits.maxTotalUncompressed - std::min(totalU, limits.maxTotalUncompressed))
                return exhausted("total uncompressed size");
            totalU += e.uncompressedSize;
            totalC = (e.compressedSize > UINT64_MAX - totalC) ? UINT64_MAX : totalC + e.compressedSize;
        }
        out.push_back(std::move(v));
    }
    // A file may not also be used as a directory prefix.
    for (const auto& v : out) {
        std::size_t pos = 0;
        while ((pos = v.foldKey.find('/', pos)) != std::string::npos) {
            if (fileKeys.count(v.foldKey.substr(0, pos))) return bad("file used as directory");
            ++pos;
        }
    }
    if (totalU > limits.ratioFloorBytes && (totalC == 0 || totalU / limits.maxRatio > totalC))
        return exhausted("overall compression ratio");
    return out;
}

ExtractionBudget::ExtractionBudget(const std::vector<ValidatedEntry>& entries, const ArchiveLimits& limits) {
    std::uint64_t sum = 0;
    for (const auto& e : entries) {
        declared_.push_back(e.isDirectory ? 0 : e.uncompressedSize);
        sum += declared_.back();
    }
    maxTotal_ = std::min(sum, limits.maxTotalUncompressed);
}

Status ExtractionBudget::begin(std::size_t entryIndex) {
    if (entryIndex >= declared_.size()) return makeError(ErrorCategory::InvalidArgument, "unknown archive entry");
    active_ = true;
    entryWritten_ = 0;
    entryDeclared_ = declared_[entryIndex];
    return okStatus();
}

Status ExtractionBudget::charge(std::uint64_t bytes) {
    if (!active_) return makeError(ErrorCategory::InvalidArgument, "no active archive entry");
    if (bytes > entryDeclared_ - std::min(entryWritten_, entryDeclared_)) return exhausted("entry larger than declared");
    if (bytes > maxTotal_ - std::min(total_, maxTotal_)) return exhausted("total larger than declared");
    entryWritten_ += bytes;
    total_ += bytes;
    return okStatus();
}

}  // namespace ss::security
