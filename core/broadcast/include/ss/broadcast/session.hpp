// StageSignal broadcast: the session state machine (spec section 15).
//   Off -> Preparing -> Connecting -> Live -> Reconnecting | Stopping -> Off, with Error as a terminal failure state.
//
// Rules enforced here (each has a REQ-named test):
//  * REQ-B02  A stream starts only with a StartToken. Tokens come from confirmPublication(), which accepts only
//             user-origin actions (Ui, Keyboard, Midi). Show load, restart recovery, benchmark and remote/system
//             sources are refused, so no automatic path can go live. A token is single use, expires, and is bound to
//             the hash of the publication summary the operator actually saw.
//  * REQ-B04  Reconnect 1/2/4/8/15 s (+-20% jitter, injected Rng and clock) inside a 120 s deadline measured from the
//             first failure of an outage. Queued media is abandoned and a keyframe requested. Certificate, auth and
//             configuration failures are terminal and never retried.
//  * REQ-B05  stop() bumps the session generation, asks the process to stop and kills it after a 750 ms grace, so the
//             state is Off within 1 s. Timer/exit callbacks carry the generation they were created in; stale ones are
//             ignored and cannot restart a stopped stream.
//  * "Live" is claimed only after outbound media has been observed (two progress samples with growing total_size and
//    out_time > 0), never merely because the process started.
//  * REQ-B03  Secrets are SecretRefs resolved at spawn; status, log and exports are redacted. The spawned argv still
//             contains the key/passphrase: that limitation is reported in BroadcastStatus::secrets, not hidden.
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "ss/base/rng.hpp"
#include "ss/base/time.hpp"
#include "ss/broadcast/command.hpp"
#include "ss/broadcast/ffmpeg_io.hpp"
#include "ss/broadcast/process.hpp"
#include "ss/broadcast/settings.hpp"

namespace ss::broadcast {

enum class State : std::uint8_t { Off, Preparing, Connecting, Live, Reconnecting, Stopping, Error };
const char* stateName(State s);

enum class ActionSource : std::uint8_t { Ui, Keyboard, Midi, Show, System, Remote };
bool isUserAction(ActionSource s);              // Ui, Keyboard, Midi only

// Proof that a person confirmed the publication summary. Only BroadcastSession::confirmPublication creates one.
class StartToken {
public:
    StartToken() = default;
    bool valid() const { return id_ != 0; }
private:
    friend class BroadcastSession;
    StartToken(std::uint64_t id, std::string hash, Ns expires) : id_(id), hash_(std::move(hash)), expires_(expires) {}
    std::uint64_t id_ = 0;
    std::string hash_;
    Ns expires_ = 0;
};

struct SessionConfig {
    Ns tokenTtl = secToNs(15);
    Ns reconnectDeadline = secToNs(120);
    Ns connectTimeout = secToNs(20);            // Connecting without outbound media for this long counts as a failure
    Ns stallTimeout = secToNs(10);              // Live without growth of total_size for this long counts as a failure
    Ns stableAfter = secToNs(30);               // Live for this long resets the outage clock and attempt counter
    Ns stopGrace = msToNs(750);                 // graceful stop window before kill (total stop budget is 1 s)
    double jitter = 0.20;                       // +-20%
};
const std::vector<Ns>& reconnectSchedule();     // 1, 2, 4, 8, 15 s (15 s repeats)

enum class SecretHandling : std::uint8_t { Partial };    // never "Solved": see note
struct SecretReport {
    SecretHandling level = SecretHandling::Partial;
    std::string note;           // what is protected and what is not (argv exposure), shown in diagnostics
};

struct StatusError { Ns at = 0; FailureKind kind = FailureKind::None; std::string message; bool terminal = false; };

struct BroadcastStatus {
    State state = State::Off;
    std::string connection;                     // operator-facing sentence, redacted
    EncryptionState encryption = EncryptionState::Unencrypted;
    std::string encryptionText;
    double bitrateKbps = 0;                     // measured from total_size deltas between progress samples
    std::uint64_t bytesSent = 0;
    std::uint64_t droppedFrames = 0;            // ffmpeg drop_frames over all processes + chunks dropped by the feed
    std::uint32_t reconnectAttempts = 0;        // in the current outage
    std::uint32_t totalReconnects = 0;
    Ns outageElapsed = 0;
    Ns nextAttemptIn = 0;                       // while Reconnecting
    std::uint64_t generation = 0;
    std::vector<StatusError> errors;            // most recent first, at most 10
    SecretReport secrets;
};

class BroadcastSession {
public:
    BroadcastSession(BroadcastSettings settings, const IClock& clock, Rng& rng, IProcessFactory& processes, IMediaFeed& feed,
                     const ISecretResolver& secrets, SessionConfig cfg = {});

    // The operator confirmed `facts` + current settings. Refused for non-user sources.
    Result<StartToken> confirmPublication(ActionSource source, const PublicationFacts& facts);
    // Starts the stream. `media` carries ffmpegPath/encoder/input endpoints; its settings field is ignored (the
    // session's own settings are used). The token must match the current summary (settings or facts changed -> refused).
    Status start(const StartToken& token, const PublicationFacts& facts, BroadcastCommandInput media);
    void stop();                                // idempotent; Off within 1 s (call tick() while Stopping)
    void tick();                                // drives timers, process polling, progress and state changes

    // Event-driven hosts may report process exit / timer expiry; stale generations are ignored.
    void onProcessExited(std::uint64_t generation, int exitCode, const std::string& stderrText);
    void onReconnectTimer(std::uint64_t generation);

    State state() const { return state_; }
    std::uint64_t generation() const { return gen_; }
    BroadcastStatus status() const;
    const std::vector<std::string>& log() const { return log_; }     // redacted
    const BroadcastSettings& settings() const { return settings_; }
    void updateSettings(const BroadcastSettings& s);                 // only while Off or Error

private:
    void setState(State s, const std::string& why);
    void note(const std::string& line);
    void recordError(const Failure& f, bool terminal);
    Status spawn();
    void handleFailure(const Failure& f);
    void scheduleReconnect();
    void pollProcess();
    void finishStop();
    bool outageExpired() const;

    BroadcastSettings settings_;
    const IClock& clock_;
    Rng& rng_;
    IProcessFactory& factory_;
    IMediaFeed& feed_;
    const ISecretResolver& secrets_;
    SessionConfig cfg_;

    State state_ = State::Off;
    std::uint64_t gen_ = 0, nextTokenId_ = 1, activeToken_ = 0;
    Ns activeTokenExpires_ = 0;
    std::string activeTokenHash_;
    std::string confirmedHash_;
    BroadcastCommand command_;
    PublicationFacts facts_;
    std::unique_ptr<IProcess> proc_;
    ProgressParser parser_;
    std::string stderrTail_;

    Ns stateSince_ = 0, outageStart_ = -1, nextAttemptAt_ = 0, stopDeadline_ = 0, liveSince_ = -1;
    Ns lastGrowthAt_ = 0, lastSampleAt_ = -1;
    std::uint64_t lastTotal_ = 0, growthSamples_ = 0, dropsBase_ = 0, dropsCur_ = 0, bytesBase_ = 0;
    std::uint32_t attempt_ = 0, totalReconnects_ = 0;
    double bitrate_ = 0;
    std::deque<StatusError> errors_;
    std::string connection_;
    std::vector<std::string> log_;
};

}  // namespace ss::broadcast
