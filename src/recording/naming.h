// MOD-11 file naming: UTC timestamp + session id + sanitized show name (spec §12). Local time is a UI concern only.
#pragma once
#include <string>
#include <cstdint>
namespace ss {
namespace rec {
// Keep [A-Za-z0-9._-]; everything else (spaces, path separators, non-ASCII bytes, control chars) -> '_'; collapse runs,
// trim, max 40 chars, never a Windows reserved device name, never empty ("show").
std::string sanitizeShowName(const std::string& in);
// Session id kept to [A-Za-z0-9-], max 24 chars, "S" if empty.
std::string sanitizeSessionId(const std::string& in);
// "20260102T030405Z" from unix ms (UTC, no locale/tz dependence, thread-safe).
std::string utcStamp(int64_t unixMs);
// "StageSignal_<utc>_<session>_<show>"  (no extension, no segment index)
std::string baseName(int64_t unixMs, const std::string& sessionId, const std::string& showName);
// "<base>_0003.<ext>"
std::string segmentName(const std::string& base, int index, const char* ext);
std::string joinPath(const std::string& dir, const std::string& name);
} // namespace rec
} // namespace ss
