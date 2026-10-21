// StageSignal broadcast: argv for the EXTERNAL ffmpeg process that encodes raw program frames + PCM and publishes
// them (RTMPS/RTMP as FLV, SRT as MPEG-TS). It reuses the input/encode pieces of ss::record::ff, so the recording
// and the stream are fed and timed the same way.
//
// The result is a list of ArgPiece: plain text, or text around a SECRET REFERENCE. The secret is resolved only when
// the process is spawned (materialize); everything shown, logged or exported uses displayLine(), which never
// contains the secret. The materialised argv still contains the secret (ffmpeg takes the key in the URL and the SRT
// passphrase as an option): see the honest limitation in docs/specs/broadcast.md (REQ-B03 is "partial").
#pragma once
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/broadcast/settings.hpp"
#include "ss/record/ffmpeg_args.hpp"
#include "ss/record/profile.hpp"

namespace ss::broadcast {

struct ArgPiece {
    std::string prefix;
    SecretRef secret;                 // empty = this piece is plain text (prefix only)
    std::string suffix;
    std::uint32_t secretMinLen = 1, secretMaxLen = 512;   // checked on the resolved value (SRT passphrase: 10..64)
    bool isSecret() const { return !secret.empty(); }
};

struct BroadcastCommandInput {
    std::string ffmpegPath = "ffmpeg";
    BroadcastSettings settings;
    record::EncoderChoice encoder;                 // must be an eligible H.264 encoder (FLV and the receivers need H.264)
    record::ff::RawVideoInput video;               // must match settings.video (the renderer delivers the stream size)
    record::ff::PcmInput audio;                    // must match settings.audio
    record::ff::InputSync sync;                    // host-clock origins + the audio delay compensation (caller supplied)
    std::uint32_t progressPeriodMs = 1000;         // -stats_period for -progress pipe:1
};

struct BroadcastCommand {
    std::vector<ArgPiece> args;                    // args[0] is the executable
    std::string container;                         // "flv" or "mpegts"
    record::ff::InputOffsets offsets;
    // Redacted single line for logs/UI: URLs keep scheme/host/port only (path and key become <redacted>), other
    // secret pieces show as <secret>.
    std::string displayLine() const;
    bool containsSecrets() const;
    // Resolves every reference now. Fails (without partial output) if a secret is missing or has an unusable form.
    Result<std::vector<std::string>> materialize(const ISecretResolver& resolver) const;
};

class FfmpegCommandBuilder {
public:
    static Result<BroadcastCommand> buildBroadcast(const BroadcastCommandInput& in);
};

}  // namespace ss::broadcast
