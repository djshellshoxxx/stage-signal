// MOD-15 portable security helpers (spec §16, REQ-B03, REQ-X04): redaction, path/protocol allowlists, import limits.
// Pure functions, no I/O (except the optional symlink probe), no Windows types. Thread-safe unless noted.
#pragma once
#include "core/types.h"
#include <string>
#include <vector>
#include <set>
#include <functional>

namespace ss { namespace sec {

constexpr const char* kRedacted = "***";

// ---- Redaction ------------------------------------------------------------------------------
// URLs: userinfo -> ***; query and fragment -> ***; for stream schemes (rtmp/rtmps/rtsp/srt/rtp/...) all path
// segments after the first (app) -> ***; for other schemes key-looking / key-introduced segments -> ***.
std::string redactUrls(const std::string& s);
// Secret-like option values: key=, streamkey:, "token":"..", --password X, Authorization: Bearer X ... -> name + ***.
std::string redactSecretOptions(const std::string& s);
// Filesystem usernames in C:\Users\<n>, /home/<n>, /Users/<n>, Documents and Settings\<n> ... -> <user>.
std::string redactUserNames(const std::string& s);
// Well-known stream key shapes (Twitch live_123_abc, YouTube xxxx-xxxx-xxxx-xxxx-xxxx) -> ***.
std::string redactKeyShapes(const std::string& s);
// All of the above, in order. Idempotent.
std::string redact(const std::string& s);
// True if an option/field NAME is secret-like (password, stream_key, apiKey, token ...): structured exporters redact its whole value.
bool isSecretKeyName(const std::string& name);
// True if text still looks like it holds a secret/URL credential (used by tests and bundle self-check).
bool looksSecret(const std::string& s);

// Redactor adds exact literals (stream keys fetched from the secret store, user/machine names) that are
// replaced anywhere, case-insensitively, before the pattern rules run. Not thread-safe for mutation.
class Redactor {
 public:
  void addLiteral(const std::string& secret, const std::string& replacement = kRedacted); // ignores len < 3
  void clearLiterals() { lits_.clear(); }
  std::string apply(const std::string& s) const;
 private:
  struct Lit { std::string match; std::string repl; };   // match is lower-cased
  std::vector<Lit> lits_;
};

// Library/OS error text -> single line, valid ASCII-safe UTF-8 subset, redacted, length-capped.
std::string sanitizeLibraryError(const std::string& raw, size_t maxLen = 512);
Status sanitizedFail(Err code, const std::string& rawDetail, bool recoverable = true);

// ---- Paths ----------------------------------------------------------------------------------
struct RelPathPolicy {
  size_t maxLen = 260;
  size_t maxSegments = 32;
  size_t maxSegmentLen = 128;
  std::vector<std::string> allowedExt;   // lower-case with dot, empty = any
  bool allowRepeatedSeparators = false;  // "a//b" rejected by default
};
// Validates an untrusted archive/show relative path. Rejects: empty, NUL/control, invalid UTF-8, absolute (/ \),
// drive (C:), UNC (\\ //), "\\?\" "\\.\" device/reparse prefixes, ':' (ADS), '..' (also dot/space-only segments),
// percent-encoded dots/separators, look-alike Unicode separators, reserved device names, trailing dot/space,
// wildcard/illegal chars, over-long names/segments, extension outside allowlist. network refs yield a clearer message.
Status validateRelPath(const std::string& p, const RelPathPolicy& pol = RelPathPolicy());
// On success writes forward-slash normalized path (no "." segments) and a case-folded key for duplicate detection.
Status canonicalizeRelPath(const std::string& p, std::string& canonical, std::string& foldedKey, const RelPathPolicy& pol = RelPathPolicy());
bool hasAllowedExtension(const std::string& relPath, const std::vector<std::string>& lowerDotExts);

// Tracks canonical paths of one archive: rejects duplicates (case-insensitive) and over-count.
class CanonicalPathSet {
 public:
  explicit CanonicalPathSet(size_t maxEntries = 10000, RelPathPolicy pol = RelPathPolicy()) : max_(maxEntries), pol_(std::move(pol)) {}
  Status add(const std::string& rel, std::string* canonicalOut = nullptr);
  size_t size() const { return keys_.size(); }
 private:
  size_t max_; RelPathPolicy pol_; std::set<std::string> keys_;
};

// Filesystem escape check: walks `rel` under `root`, rejecting symlinks (std::filesystem) and anything `isReparse`
// (injected: the Windows adapter supplies a junction/reparse-point probe) reports. Missing components are fine.
using ReparseProbe = std::function<bool(const std::string& absPath)>;
Status checkNoLinkEscape(const std::string& root, const std::string& rel, const ReparseProbe& isReparse = ReparseProbe());

// ---- Protocols ------------------------------------------------------------------------------
// Lower-case scheme if s (after trimming leading whitespace/control) starts with "scheme:" (scheme len >= 2), else "".
std::string urlScheme(const std::string& s);
bool isNetworkScheme(const std::string& scheme);
// Local content (show media references, presets, fixtures) must be relative archive paths: any scheme, "://",
// pipe/concat tricks, UNC or absolute forms are rejected, so local content can never name a network URL.
Status checkLocalContentRef(const std::string& ref, const RelPathPolicy& pol = RelPathPolicy());
// Stream adapter allowlist (separately owned from decoders): rtmps, srt always; rtp only if allowRtp; rtmp only if allowInsecureRtmp.
Status checkStreamUrlScheme(const std::string& url, bool allowRtp = false, bool allowInsecureRtmp = false);
// Decoder protocol allowlist for local files: only "file"-less plain paths; returns names a decoder may enable.
const std::vector<std::string>& localDecoderProtocolAllowlist();   // {"file"}  (exported to the media adapter)
// Loopback/discard-only endpoints (benchmark isolation, REQ-P02).
bool isLoopbackOrDiscardEndpoint(const std::string& endpoint);

// ---- Import limits (spec §17) -----------------------------------------------------------------
struct ImportLimits {
  uint64_t maxJsonBytes = 8ull << 20; uint32_t maxCues = 10000, maxScenes = 1000, maxMessages = 256,
           maxMidiMappings = 512, maxFixtures = 256, maxArchiveEntries = 10000;
  uint64_t maxDecompressedBytes = 2ull << 30; uint32_t maxCompressionRatio = 200;  // per entry, anti-bomb
};
Status checkCount(const char* what, uint64_t n, uint64_t limit);
// Overflow-safe running total + per-entry ratio check; call before extraction/allocation.
Status checkDecompressed(uint64_t& runningTotal, uint64_t entryUncompressed, uint64_t entryCompressed, const ImportLimits& lim = ImportLimits());

}} // namespace ss::sec
