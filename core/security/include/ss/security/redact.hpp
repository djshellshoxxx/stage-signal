// StageSignal security: redaction of anything that may reach logs, errors, exports or support bundles
// (spec section 16, REQ-B03). Pure functions, no I/O, idempotent: redact(redact(x)) == redact(x).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace ss::security {

constexpr const char* kRedactedMarker = "<redacted>";
constexpr const char* kUserMarker = "<user>";

struct RedactionResult {
    std::string text;
    int replacements = 0;   // how many secret-like spans were rewritten
};

// Rewrites, in order:
//  1. URLs: strips userinfo, query and fragment; for rtmp/rtmps/rtsp/srt keeps only the first path segment
//     (the app name) and replaces deeper segments (stream keys); for other schemes replaces key-looking segments.
//  2. Filesystem usernames in /Users/<n>/, /home/<n>/, C:\Users\<n>\ -> <user>.
//  3. Secret-like option values: key=, token=, passphrase=, password=, secret=, auth=, "password": "...",
//     CLI form -passphrase value, and "Bearer <token>" / "Basic <token>".
RedactionResult redact(const std::string& in);
inline std::string redactText(const std::string& in) { return redact(in).text; }
inline bool containsSecret(const std::string& in) { return redact(in).replacements > 0; }

// Redacts one URL-ish string (same rules as the URL step above, then the generic rules).
std::string redactUrl(const std::string& url);

// Makes free text safe for a single log/journal line: redacts, replaces control characters with a space,
// replaces invalid UTF-8 high bytes with '?', and truncates at a code point boundary to maxBytes.
std::string sanitizeForLog(const std::string& in, std::size_t maxBytes = 256);

// Cuts at a UTF-8 code point boundary (never splits a sequence). Valid input stays valid.
std::string truncateUtf8(const std::string& in, std::size_t maxBytes);

// Scrubs literal secret values (for example everything currently in an ISecretStore) in addition to the
// pattern rules. Literals shorter than 4 bytes are ignored to avoid shredding ordinary text.
class Redactor {
public:
    void addLiteral(const std::string& secret);
    std::size_t literalCount() const { return literals_.size(); }
    RedactionResult apply(const std::string& in) const;
private:
    std::vector<std::string> literals_;
};

}  // namespace ss::security
