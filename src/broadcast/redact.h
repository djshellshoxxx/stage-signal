// MOD-12 private redactor (self-contained; deliberately independent of core/security.h).
// Strips userinfo, query/fragment and key-like path segments from URLs and secret-like option values from free text.
#pragma once
#include <string>
namespace ss {
namespace cast {
// "rtmps://user:pw@host:443/live/sk_abc123?token=x#f" -> "rtmps://host:443/live/<redacted>?<redacted>"
std::string redactUrl(const std::string& url);
// Redacts every URL inside free text and "key=value"/"key: value" pairs whose key looks secret-like.
std::string redactText(const std::string& text);
// redactText() plus exact removal of a known secret (defence in depth for transport-supplied strings).
std::string redactWithSecret(const std::string& text, const std::string& secret);
// Best-effort overwrite of a secret held in memory.
void wipe(std::string& s);
} // namespace cast
} // namespace ss
