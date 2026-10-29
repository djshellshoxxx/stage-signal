#include "App/Media/MediaTypes.h"
#include <algorithm>
#include <cstring>

namespace ss::app {

void fitRgba(const ss::PixelBuffer& src, std::uint32_t dw, std::uint32_t dh, std::vector<std::uint8_t>& out) {
    out.assign(static_cast<std::size_t>(dw) * dh * 4, 0);
    for (std::size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    if (src.empty() || dw == 0 || dh == 0) return;
    if (src.width == dw && src.height == dh) { std::memcpy(out.data(), src.rgba.data(), out.size()); return; }
    const double sx = static_cast<double>(dw) / src.width, sy = static_cast<double>(dh) / src.height, sc = std::min(sx, sy);
    const std::uint32_t tw = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(src.width * sc)), th = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(src.height * sc));
    const std::uint32_t ox = (dw - tw) / 2, oy = (dh - th) / 2;
    for (std::uint32_t y = 0; y < th; ++y) {
        const std::uint8_t* srow = src.row(std::min<std::uint32_t>(src.height - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * src.height / th)));
        std::uint8_t* drow = out.data() + (static_cast<std::size_t>(oy + y) * dw + ox) * 4;
        for (std::uint32_t x = 0; x < tw; ++x) {
            const std::uint32_t sxp = std::min<std::uint32_t>(src.width - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * src.width / tw));
            std::memcpy(drow + x * 4, srow + sxp * 4, 3);
        }
    }
}

void remapChannels(const AudioChunk& in, std::uint32_t outCh, std::vector<float>& out) {
    out.resize(static_cast<std::size_t>(in.frames) * outCh);
    for (std::uint32_t f = 0; f < in.frames; ++f)
        for (std::uint32_t c = 0; c < outCh; ++c)
            out[static_cast<std::size_t>(f) * outCh + c] = in.s[static_cast<std::size_t>(f) * in.channels + (c < in.channels ? c : in.channels - 1)];
}

ss::record::ProbeResults toProbeResults(const FfmpegProbe& p) {
    ss::record::ProbeResults r;
    r.ffmpegPresent = p.found;
    r.ffmpegVersion = p.versionLine;
    r.encoders = ss::record::parseFfmpegEncoders(p.encodersText);
    for (auto& e : r.encoders)
        e.verified = std::find(p.verifiedEncoders.begin(), p.verifiedEncoders.end(), e.name) != p.verifiedEncoders.end();
    return r;
}

}  // namespace ss::app
