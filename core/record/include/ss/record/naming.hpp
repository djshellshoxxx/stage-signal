// StageSignal record: file naming (spec section 12: "File names contain UTC timestamp/session ID and sanitized
// show name; local UI displays local time"). ASCII-only names are portable across NTFS/APFS/ext4/exFAT/FAT32.
#pragma once
#include <cstdint>
#include <string>
#include "ss/base/rng.hpp"
#include "ss/base/time.hpp"

namespace ss::record {

// Keeps [A-Za-z0-9._-], maps everything else (spaces, separators, non-ASCII, control chars) to '_', collapses runs,
// trims leading/trailing '_' '.' ' ', avoids Windows reserved device names, caps at maxBytes. Never empty ("show").
std::string sanitizeShowName(const std::string& utf8, std::size_t maxBytes = 48);

// 20261020T153045Z (UTC, second resolution) from Unix nanoseconds.
std::string formatUtcCompact(Ns wallUnixNs);

// 8 lowercase hex characters from the deterministic Rng.
std::string makeSessionId(Rng& rng);
bool isValidSessionId(const std::string& id);

// <UTC>_<session>_<show>_seg<NNNN>.<ext> ; index is 1-based.
std::string segmentFileName(Ns sessionStartWallUnixNs, const std::string& sessionId, const std::string& showName,
                            std::uint32_t segmentIndex, const std::string& ext);
// Same stem for ffmpeg's segment muxer with a printf-style index (%04d, paired with -segment_start_number 1).
// Any '%' inside the stem is escaped as "%%".
std::string segmentPattern(Ns sessionStartWallUnixNs, const std::string& sessionId, const std::string& showName,
                           const std::string& ext);
std::string journalFileName(Ns sessionStartWallUnixNs, const std::string& sessionId, const std::string& showName);
std::string segmentListFileName(Ns sessionStartWallUnixNs, const std::string& sessionId, const std::string& showName);
std::string metadataFileName(Ns sessionStartWallUnixNs, const std::string& sessionId, const std::string& showName);

}  // namespace ss::record
