#include "ss/broadcast/command.hpp"
#include <algorithm>
#include "ss/broadcast/redact.hpp"

namespace ss::broadcast {
namespace {
Error bad(std::string d) { return makeError(ErrorCategory::InvalidArgument, std::move(d), Recoverability::OperatorAction); }

void plain(std::vector<ArgPiece>& out, const std::string& s) { ArgPiece p; p.prefix = s; out.push_back(std::move(p)); }
void plainAll(std::vector<ArgPiece>& out, const std::vector<std::string>& v) { for (const auto& s : v) plain(out, s); }

std::string hostPort(const BroadcastSettings& s) {
    const std::uint32_t port = effectivePort(s);
    return s.host + (port ? ":" + std::to_string(port) : std::string());
}
bool needsQuote(const std::string& s) {
    return s.empty() || std::any_of(s.begin(), s.end(), [](char c) { return c == ' ' || c == '"' || c == '\'' || c == '\\'; });
}
}  // namespace

std::string BroadcastCommand::displayLine() const {
    std::string line;
    for (const auto& a : args) {
        std::string shown;
        if (a.prefix.find("://") != std::string::npos) shown = redactUrl(a.prefix + (a.isSecret() ? "KEY" : "") + a.suffix);
        else shown = a.prefix + (a.isSecret() ? "<secret>" : "") + a.suffix;
        if (!line.empty()) line += ' ';
        line += needsQuote(shown) ? "\"" + shown + "\"" : shown;
    }
    return line;
}
bool BroadcastCommand::containsSecrets() const {
    return std::any_of(args.begin(), args.end(), [](const ArgPiece& a) { return a.isSecret(); });
}

Result<std::vector<std::string>> BroadcastCommand::materialize(const ISecretResolver& resolver) const {
    std::vector<std::string> argv;
    argv.reserve(args.size());
    for (const auto& a : args) {
        if (!a.isSecret()) { argv.push_back(a.prefix + a.suffix); continue; }
        auto r = resolver.resolve(a.secret);
        if (!r.ok()) { wipe(argv); return makeError(ErrorCategory::PermissionDenied, "A stored secret could not be read. Re-enter it in the secret store.", Recoverability::OperatorAction); }
        std::string v = r.value();
        const bool control = std::any_of(v.begin(), v.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
        const bool ok = !control && v.size() >= a.secretMinLen && v.size() <= a.secretMaxLen;
        if (!ok) {
            std::vector<std::string> tmp{v};
            wipe(tmp);
            wipe(argv);
            return makeError(ErrorCategory::InvalidArgument, "A stored secret has an unusable form (length or characters). Re-enter it in the secret store.", Recoverability::OperatorAction);
        }
        argv.push_back(a.prefix + v + a.suffix);
        std::vector<std::string> tmp{v};
        wipe(tmp);
    }
    return argv;
}

Result<BroadcastCommand> FfmpegCommandBuilder::buildBroadcast(const BroadcastCommandInput& in) {
    const auto& s = in.settings;
    if (in.ffmpegPath.empty() || in.ffmpegPath[0] == '-') return bad("ffmpeg path is empty or looks like an option");
    if (availability(s.profile) == Availability::Later || availability(s.profile) == Availability::Deferred)
        return makeError(ErrorCategory::UnsupportedCapability, std::string(profileName(s.profile)) + " output is not available in this build", Recoverability::OperatorAction);
    const auto issues = validateSettings(s, false);
    if (hasBlocking(issues)) {
        for (const auto& i : issues) if (i.blocking) return bad(i.field + ": " + i.message);
    }
    if (in.encoder.codec != record::VideoCodec::H264 || in.encoder.name == "mjpeg" || !record::isEligibleEncoder(in.encoder.name) ||
        record::isExcludedGplEncoder(in.encoder.name))
        return makeError(ErrorCategory::EncoderUnavailable, "Streaming needs a verified, eligible H.264 encoder (not MJPEG, not libx264).", Recoverability::OperatorAction);
    if (in.video.width != s.video.width || in.video.height != s.video.height || in.video.fpsNum != s.video.fps || in.video.fpsDen != 1)
        return bad("video input does not match the stream settings");
    if (in.audio.rateHz != s.audio.rateHz || in.audio.channels != s.audio.channels) return bad("audio input does not match the stream settings");

    BroadcastCommand cmd;
    plain(cmd.args, in.ffmpegPath);
    plainAll(cmd.args, record::ff::globalArgs());
    cmd.offsets = record::ff::computeInputOffsets(in.sync);
    {
        std::vector<std::string> inputs;
        Status st = record::ff::appendInputs(inputs, in.video, in.audio, cmd.offsets);
        if (!st.ok()) return st.error();
        plainAll(cmd.args, inputs);
    }
    {
        std::vector<std::string> enc;
        record::ff::VideoEncodeSpec v;
        v.encoder = in.encoder.name; v.codec = record::VideoCodec::H264; v.bitrateKbps = s.video.bitrateKbps;
        v.keyintSeconds = s.video.keyframeSeconds; v.fpsNum = s.video.fps; v.fpsDen = 1;
        record::ff::appendVideoEncode(enc, v);
        record::ff::appendForcedKeyframes(enc, s.video.keyframeSeconds);          // a keyframe exactly every N s, also after a restart
        record::ff::AudioEncodeSpec a;
        a.codec = record::ff::AudioCodec::Aac; a.bitrateKbps = s.audio.bitrateKbps; a.rateHz = s.audio.rateHz; a.channels = s.audio.channels;
        record::ff::appendAudioEncode(enc, a);
        plainAll(cmd.args, enc);
    }
    plainAll(cmd.args, {"-progress", "pipe:1", "-stats_period", record::ff::secondsArg(msToNs(std::max<std::uint32_t>(250, in.progressPeriodMs)))});

    if (s.profile == Profile::Rtmps || s.profile == Profile::Rtmp) {
        cmd.container = "flv";
        if (s.profile == Profile::Rtmps) {
            // ffmpeg's tls_verify defaults to 0 (no validation). It MUST be forced on, with the expected host name.
            plainAll(cmd.args, {"-tls_verify", "1", "-verifyhost", s.host});
        }
        plainAll(cmd.args, {"-f", "flv"});
        ArgPiece url;
        url.prefix = std::string(s.profile == Profile::Rtmps ? "rtmps://" : "rtmp://") + hostPort(s) + "/" + s.path + "/";
        url.secret = s.streamKey;
        cmd.args.push_back(std::move(url));
    } else {
        cmd.container = "mpegts";
        plainAll(cmd.args, {"-mode", "caller", "-latency", std::to_string(static_cast<std::uint64_t>(s.srtLatencyMs) * 1000ULL),   // ffmpeg: microseconds
                            "-connect_timeout", std::to_string(s.srtConnectTimeoutMs)});                                         // ffmpeg: milliseconds
        if (!s.srtPassphrase.empty()) {
            plain(cmd.args, "-passphrase");
            ArgPiece pw; pw.secret = s.srtPassphrase; pw.secretMinLen = 10; pw.secretMaxLen = 64;      // libsrt: 10..64 characters
            cmd.args.push_back(std::move(pw));
            plainAll(cmd.args, {"-pbkeylen", std::to_string(s.srtKeyLenBytes), "-enforced_encryption", "1"});
        }
        plainAll(cmd.args, {"-mpegts_flags", "+resend_headers", "-f", "mpegts"});
        plain(cmd.args, "srt://" + hostPort(s));
    }
    // Structural check on everything that is not secret.
    std::vector<std::string> shape;
    for (const auto& a : cmd.args) shape.push_back(a.prefix + a.suffix);
    Status chk = record::ff::checkArgv(shape);
    if (!chk.ok()) return chk.error();
    return cmd;
}

}  // namespace ss::broadcast
