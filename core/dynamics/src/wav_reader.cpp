#include "ss/dynamics/wav_reader.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ss::dynamics {

namespace {
std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
void put32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16); p[3] = static_cast<std::uint8_t>(v >> 24);
}

struct Plan {
    WavInfo info;
    std::uint64_t frames = 0, partial = 0, newFileBytes = 0;
    WavRecoveryReport report;
};

Result<Plan> makePlan(const std::uint8_t* hdr, std::size_t n, std::uint64_t fileBytes, const RecoveryHints& hints) {
    auto p = parseWavHeader(hdr, n, fileBytes);
    if (!p.ok()) return p.error();
    Plan pl;
    pl.info = p.value();
    const std::uint16_t ba = pl.info.blockAlign();
    if (ba == 0) return makeError(ErrorCategory::DecodeFailure, "invalid WAV format (zero block size)");
    const std::uint64_t avail = pl.info.availableDataBytes;
    pl.frames = avail / ba;
    pl.partial = avail % ba;
    pl.newFileBytes = pl.info.dataOffset + pl.frames * ba;
    WavRecoveryReport& r = pl.report;
    r.headerWasConsistent = pl.info.consistent(fileBytes);
    const bool placeholder = pl.info.claimedDataBytes == 0xFFFFFFFFULL;
    r.claimedFrames = placeholder ? 0 : pl.info.claimedDataBytes / ba;
    r.framesRecovered = pl.frames;
    r.partialFrameBytesDropped = pl.partial;
    r.secondsRecovered = static_cast<double>(pl.frames) / pl.info.sampleRateHz;
    char buf[320];
    if (hints.hasExpectedFrames) {
        r.lostTailKnown = true;
        r.framesLostAtLeast = hints.expectedFrames > pl.frames ? hints.expectedFrames - pl.frames : 0;
        std::snprintf(buf, sizeof buf, "Recovered %llu frames (%.3f s). Journal says %llu frames were flushed; %llu frames (%.3f s) are missing. Audio after the last flush is not recoverable.",
                      static_cast<unsigned long long>(pl.frames), r.secondsRecovered,
                      static_cast<unsigned long long>(hints.expectedFrames), static_cast<unsigned long long>(r.framesLostAtLeast),
                      static_cast<double>(r.framesLostAtLeast) / pl.info.sampleRateHz);
    } else {
        std::snprintf(buf, sizeof buf, "Recovered %llu frames (%.3f s). Audio written after the last flush may be missing; the amount is unknown without a journal figure.",
                      static_cast<unsigned long long>(pl.frames), r.secondsRecovered);
    }
    r.disclosure = buf;
    return pl;
}
}  // namespace

bool WavInfo::consistent(std::uint64_t fileBytes) const {
    const std::uint16_t ba = blockAlign();
    return ba != 0 && claimedDataBytes == availableDataBytes && availableDataBytes % ba == 0 &&
           claimedRiffBytes + 8 == fileBytes;
}

Result<WavInfo> parseWavHeader(const std::uint8_t* d, std::size_t n, std::uint64_t total) {
    if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0)
        return makeError(ErrorCategory::DecodeFailure, "not a RIFF/WAVE file");
    WavInfo w;
    w.claimedRiffBytes = get32(d + 4);
    bool haveFmt = false;
    std::uint64_t pos = 12;
    while (pos + 8 <= n) {
        const std::uint32_t sz = get32(d + pos + 4);
        if (std::memcmp(d + pos, "fmt ", 4) == 0) {
            if (sz < 16 || pos + 8 + 16 > n) return makeError(ErrorCategory::DecodeFailure, "truncated fmt chunk");
            if (get16(d + pos + 8) != 1) return makeError(ErrorCategory::UnsupportedCapability, "only integer PCM WAV is supported");
            w.channels = get16(d + pos + 10);
            w.sampleRateHz = get32(d + pos + 12);
            w.bitsPerSample = get16(d + pos + 22);
            if (w.channels == 0 || w.sampleRateHz == 0 || (w.bitsPerSample != 16 && w.bitsPerSample != 24))
                return makeError(ErrorCategory::UnsupportedCapability, "unsupported PCM format (need 16 or 24 bit)");
            haveFmt = true;
        } else if (std::memcmp(d + pos, "data", 4) == 0) {
            if (!haveFmt) return makeError(ErrorCategory::DecodeFailure, "data chunk before fmt chunk");
            w.dataOffset = pos + 8;
            w.claimedDataBytes = sz;
            w.availableDataBytes = total > w.dataOffset ? total - w.dataOffset : 0;
            return w;
        }
        pos += 8 + static_cast<std::uint64_t>(sz) + (sz & 1u);
    }
    return makeError(ErrorCategory::DecodeFailure, "no audio data chunk found");
}

Result<WavData> readWav(const std::vector<std::uint8_t>& file) {
    auto p = parseWavHeader(file.data(), file.size(), file.size());
    if (!p.ok()) return p.error();
    WavData w;
    w.info = p.value();
    if (!w.info.consistent(file.size()))
        return makeError(ErrorCategory::DecodeFailure, "WAV header sizes do not match file length (truncated or unfinalised)");
    const std::size_t bytes = w.info.bitsPerSample / 8;
    const std::size_t count = static_cast<std::size_t>(w.info.availableDataBytes / bytes);
    w.samples.resize(count);
    const std::uint8_t* s = file.data() + w.info.dataOffset;
    for (std::size_t i = 0; i < count; ++i, s += bytes) {
        if (bytes == 3) {
            std::int32_t v = static_cast<std::int32_t>(s[0]) | (static_cast<std::int32_t>(s[1]) << 8) | (static_cast<std::int32_t>(s[2]) << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;
            w.samples[i] = v;
        } else {
            w.samples[i] = static_cast<std::int16_t>(get16(s));
        }
    }
    return w;
}

std::vector<float> wavToFloat(const WavData& w) {
    const float scale = 1.0f / static_cast<float>(1u << (w.info.bitsPerSample - 1));
    std::vector<float> out(w.samples.size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<float>(w.samples[i]) * scale;
    return out;
}

Result<WavRecoveryReport> recoverWav(std::vector<std::uint8_t>& file, const RecoveryHints& hints) {
    auto pl = makePlan(file.data(), file.size(), file.size(), hints);
    if (!pl.ok()) return pl.error();
    const Plan& p = pl.value();
    if (p.newFileBytes < file.size()) file.resize(static_cast<std::size_t>(p.newFileBytes));
    const std::uint64_t dataBytes = p.frames * p.info.blockAlign();
    if (dataBytes > 0xFFFFFFFFULL - 36) return makeError(ErrorCategory::UnsupportedCapability, "data exceeds classic RIFF limit");
    put32(file.data() + 4, static_cast<std::uint32_t>(p.newFileBytes - 8));
    put32(file.data() + p.info.dataOffset - 4, static_cast<std::uint32_t>(dataBytes));
    return p.report;
}

Result<WavRecoveryReport> recoverWavFile(const std::string& path, const RecoveryHints& hints) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path fp = fs::u8path(path);
    const std::uint64_t size = fs::file_size(fp, ec);
    if (ec) return makeError(ErrorCategory::DecodeFailure, "cannot stat recording file");
    std::vector<std::uint8_t> hdr(static_cast<std::size_t>(std::min<std::uint64_t>(size, 4096)));
    {
        std::ifstream in(fp, std::ios::binary);
        if (!in) return makeError(ErrorCategory::PermissionDenied, "cannot open recording file");
        in.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
        if (!in && !in.eof()) return makeError(ErrorCategory::DecodeFailure, "cannot read recording file");
    }
    auto pl = makePlan(hdr.data(), hdr.size(), size, hints);
    if (!pl.ok()) return pl.error();
    const Plan& p = pl.value();
    const std::uint64_t dataBytes = p.frames * p.info.blockAlign();
    if (dataBytes > 0xFFFFFFFFULL - 36) return makeError(ErrorCategory::UnsupportedCapability, "data exceeds classic RIFF limit");
    if (p.newFileBytes < size) {
        fs::resize_file(fp, p.newFileBytes, ec);
        if (ec) return makeError(ErrorCategory::DiskFull, "cannot truncate recording file");
    }
    std::fstream f(fp, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) return makeError(ErrorCategory::PermissionDenied, "cannot reopen recording file for repair");
    std::uint8_t b[4];
    put32(b, static_cast<std::uint32_t>(p.newFileBytes - 8));
    f.seekp(4); f.write(reinterpret_cast<const char*>(b), 4);
    put32(b, static_cast<std::uint32_t>(dataBytes));
    f.seekp(static_cast<std::streamoff>(p.info.dataOffset - 4)); f.write(reinterpret_cast<const char*>(b), 4);
    f.flush();
    if (!f) return makeError(ErrorCategory::DiskFull, "cannot rewrite recording header");
    return p.report;
}

}  // namespace ss::dynamics
