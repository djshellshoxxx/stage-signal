// StageSignal broadcast: redaction of stream URLs and secrets before anything reaches a log, status line, export or
// diagnostics bundle (REQ-B03, spec section 15). ffmpeg echoes the full output URL (including the stream key) in its
// own error messages, so everything read from the process is redacted before it is stored.
#pragma once
#include <string>
#include <vector>

namespace ss::broadcast {

// "rtmps://user:pw@host:443/app/KEY?x=1" -> "rtmps://host:443/<redacted>?<redacted>". Userinfo, path and query are
// removed; scheme, host and port are kept because the operator needs them to diagnose a connection.
std::string redactUrl(const std::string& url);
// Redacts every "scheme://..." token in free text and every literal occurrence of the given secrets (length >= 4).
std::string redactText(const std::string& text, const std::vector<std::string>& knownSecrets = {});
// Overwrites the characters of each string (best effort; the optimiser may not be prevented from eliding copies).
void wipe(std::vector<std::string>& values);

}  // namespace ss::broadcast
