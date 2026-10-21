// StageSignal broadcast: interpreting what an external ffmpeg process tells us (stderr text, exit code and the
// `-progress pipe:1` key=value stream). Patterns come from real ffmpeg 6.1 output (docs/specs/broadcast.md); text
// matching is the only channel ffmpeg offers, so unknown text is "Unknown, retryable" and never silently terminal.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/time.hpp"

namespace ss::broadcast {

enum class FailureKind : std::uint8_t {
    None,
    Certificate,        // TLS peer certificate / host name verification failed        (terminal)
    Auth,               // stream key rejected, 401/403, publish denied                  (terminal)
    InvalidConfig,      // unknown option/encoder, incompatible codec, bad passphrase   (terminal)
    Dns,                // host name does not resolve                                    (retry)
    Refused,            // connection refused                                            (retry)
    Timeout,            // connect or I/O timeout                                        (retry)
    ConnectionLost,     // broken pipe, reset, EOF, handshake not answered              (retry)
    Stalled,            // no outbound bytes for the stall window (detected here)       (retry)
    Unknown             // anything else                                                 (retry, bounded by the deadline)
};
const char* failureName(FailureKind k);
bool isTerminal(FailureKind k);                         // Certificate, Auth, InvalidConfig: never retried (REQ-B04)

struct Failure {
    FailureKind kind = FailureKind::None;
    std::string message;                                // redacted, operator-facing
};
// Scans stderr text (already read from the process). Most specific terminal kind wins over retryable ones.
Failure classifyStderr(const std::string& stderrText);
// Combines the text with the exit code: exit 8 ("Option not found") and a non-zero code with no text.
Failure classifyExit(int exitCode, const std::string& stderrText);

struct ProgressSample {
    std::uint64_t frame = 0;
    std::uint64_t totalSize = 0;                        // bytes the muxer has written
    Ns outTime = -1;                                    // -1 when ffmpeg reports N/A
    std::uint64_t dropFrames = 0, dupFrames = 0;
    double bitrateKbps = -1;                            // ffmpeg's own average, -1 when N/A
    bool end = false;                                   // progress=end
};

// Incremental parser for `-progress pipe:1`: feed arbitrary chunks, completed blocks are returned.
class ProgressParser {
public:
    std::vector<ProgressSample> feed(const std::string& chunk);
private:
    std::string pending_;
    ProgressSample cur_;
};

}  // namespace ss::broadcast
