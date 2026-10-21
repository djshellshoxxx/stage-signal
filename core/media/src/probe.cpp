#include "ss/media/probe.hpp"
#include <algorithm>
#include <cstring>

namespace ss::media {
namespace {

bool startsWith(const std::uint8_t* d, std::size_t n, const char* sig, std::size_t sigLen, std::size_t at = 0) {
    return n >= at + sigLen && std::memcmp(d + at, sig, sigLen) == 0;
}
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint32_t le32(const std::uint8_t* p) {
    return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) | p[0];
}
std::uint32_t le16(const std::uint8_t* p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8); }

bool containsCI(const std::uint8_t* d, std::size_t n, const char* needle) {
    const std::size_t m = std::strlen(needle);
    if (n < m) return false;
    for (std::size_t i = 0; i + m <= n; ++i) {
        std::size_t k = 0;
        for (; k < m; ++k) {
            unsigned char c = d[i + k];
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
            if (c != static_cast<unsigned char>(needle[k])) break;
        }
        if (k == m) return true;
    }
    return false;
}

bool looksLikeActiveContent(const std::uint8_t* d, std::size_t n) {
    n = std::min<std::size_t>(n, 512);
    std::size_t i = 0;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) i = 3;       // UTF-8 BOM
    if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) return true;  // UTF-16 text
    while (i < n && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n')) ++i;
    if (i < n && d[i] == '<') return true;
    static const char* const needles[] = {"<svg", "<html", "<script", "<?xml", "<!doctype", "javascript:"};
    for (const char* needle : needles) if (containsCI(d, n, needle)) return true;
    return false;
}

SniffResult accept(Format f) { SniffResult r; r.format = f; return r; }
SniffResult reject(SniffReject why, const char* detail) {
    SniffResult r; r.reject = why; r.detail = detail; return r;
}

bool brandIn(const std::uint8_t* b, const char* const* list, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) if (std::memcmp(b, list[i], 4) == 0) return true;
    return false;
}

}  // namespace

SniffResult sniff(const std::uint8_t* d, std::size_t n) {
    if (d == nullptr || n < 4) return reject(SniffReject::TooShort, "too short to identify");
    if (startsWith(d, n, "\x89PNG\r\n\x1a\n", 8)) return accept(Format::Png);
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return accept(Format::Jpeg);
    if (startsWith(d, n, "GIF87a", 6) || startsWith(d, n, "GIF89a", 6)) return accept(Format::Gif);
    if (startsWith(d, n, "BM", 2)) {
        // BMP magic is only two bytes: also require a plausible DIB header size at offset 14.
        if (n >= 18) {
            const std::uint32_t dib = le32(d + 14);
            if (dib == 12 || dib == 40 || dib == 52 || dib == 56 || dib == 64 || dib == 108 || dib == 124)
                return accept(Format::Bmp);
        }
        if (n < 18) return reject(SniffReject::TooShort, "bmp header truncated");
        return reject(SniffReject::UnknownSignature, "bmp signature without valid header");
    }
    if (n >= 12 && std::memcmp(d + 4, "ftyp", 4) == 0) {
        static const char* const mp4Brands[] = {"isom", "iso2", "iso4", "iso5", "iso6", "mp41", "mp42", "avc1", "M4V ", "dash"};
        static const char* const movBrands[] = {"qt  "};
        if (brandIn(d + 8, movBrands, 1)) return accept(Format::Mov);
        if (brandIn(d + 8, mp4Brands, sizeof mp4Brands / sizeof *mp4Brands)) return accept(Format::Mp4);
        return reject(SniffReject::UnsupportedBrand, "ISO base media brand is not on the allowlist");
    }
    if (n >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3) {
        const std::size_t scan = std::min<std::size_t>(n, 64);
        const char* mk = "matroska";
        for (std::size_t i = 4; i + 8 <= scan; ++i)
            if (std::memcmp(d + i, mk, 8) == 0) return accept(Format::Mkv);
        if (n < 64) return reject(SniffReject::TooShort, "matroska header truncated");
        return reject(SniffReject::UnsupportedBrand, "EBML doctype is not matroska");
    }
    if (looksLikeActiveContent(d, n)) return reject(SniffReject::ActiveContent, "markup/script content is not allowed");
    return reject(SniffReject::UnknownSignature, "unknown file signature");
}

Status checkDeclaredFormat(const std::string& declared, const SniffResult& sniffed) {
    if (!sniffed.ok())
        return makeError(ErrorCategory::DecodeFailure, "media rejected: " + sniffed.detail, Recoverability::OperatorAction);
    const Format want = formatFromDeclared(declared);
    if (want == Format::Unknown)
        return makeError(ErrorCategory::InvalidArgument, "declared media format is not on the allowlist", Recoverability::OperatorAction);
    if (want != sniffed.format)
        return makeError(ErrorCategory::InvalidArgument,
                         std::string("declared format '") + declared + "' does not match content (" + formatName(sniffed.format) + ")",
                         Recoverability::OperatorAction);
    return okStatus();
}

Result<ImageHeader> readImageHeader(const std::uint8_t* d, std::size_t n, Format f) {
    auto bad = [](const char* why) { return makeError(ErrorCategory::DecodeFailure, why, Recoverability::OperatorAction); };
    ImageHeader h;
    switch (f) {
        case Format::Png: {
            if (n < 24 || be32(d + 8) != 13 || std::memcmp(d + 12, "IHDR", 4) != 0) return bad("png header invalid");
            h.width = be32(d + 16); h.height = be32(d + 20);
            break;
        }
        case Format::Bmp: {
            if (n < 26) return bad("bmp header truncated");
            const std::uint32_t dib = le32(d + 14);
            if (dib == 12) { h.width = le16(d + 18); h.height = le16(d + 20); }
            else {
                if (n < 26) return bad("bmp header truncated");
                const std::int32_t w = static_cast<std::int32_t>(le32(d + 18));
                const std::int32_t hh = static_cast<std::int32_t>(le32(d + 22));
                if (w <= 0 || hh == INT32_MIN) return bad("bmp dimensions invalid");
                h.width = static_cast<std::uint32_t>(w);
                h.height = static_cast<std::uint32_t>(hh < 0 ? -static_cast<std::int64_t>(hh) : hh);   // negative = top-down
            }
            break;
        }
        case Format::Jpeg: {
            std::size_t i = 2;
            bool found = false;
            while (i + 4 <= n) {
                if (d[i] != 0xFF) { ++i; continue; }
                while (i < n && d[i] == 0xFF) ++i;                 // fill bytes
                if (i >= n) break;
                const std::uint8_t m = d[i++];
                if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0x00) continue;   // standalone markers
                if (m == 0xD9 || m == 0xDA) break;                 // EOI / SOS before any SOF
                if (i + 2 > n) break;
                const std::size_t len = (std::size_t(d[i]) << 8) | d[i + 1];
                if (len < 2) return bad("jpeg segment length invalid");
                const bool sof = (m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC;
                if (sof) {
                    if (len < 8 || i + 7 > n) return bad("jpeg frame header truncated");
                    h.height = (std::uint32_t(d[i + 3]) << 8) | d[i + 4];
                    h.width = (std::uint32_t(d[i + 5]) << 8) | d[i + 6];
                    found = true;
                    break;
                }
                i += len;
            }
            if (!found) return bad("jpeg frame header not found");
            break;
        }
        default: return makeError(ErrorCategory::InvalidArgument, "not a still image format");
    }
    if (h.width == 0 || h.height == 0) return bad("image dimensions are zero");
    return h;
}

Result<GifInfo> parseGif(const std::uint8_t* d, std::size_t n) {
    auto bad = [](const char* why) { return makeError(ErrorCategory::DecodeFailure, why, Recoverability::OperatorAction); };
    if (n < 13 || !(startsWith(d, n, "GIF87a", 6) || startsWith(d, n, "GIF89a", 6))) return bad("gif header invalid");
    GifInfo info;
    info.width = le16(d + 6); info.height = le16(d + 8);
    if (info.width == 0 || info.height == 0) return bad("gif dimensions are zero");
    std::size_t i = 13;
    if (d[10] & 0x80) i += 3u * (std::size_t(1) << ((d[10] & 7) + 1));
    GifFrameInfo pending;
    bool havePending = false;
    auto skipSubBlocks = [&](std::size_t& pos) -> bool {
        while (true) {
            if (pos >= n) return false;
            const std::size_t len = d[pos++];
            if (len == 0) return true;
            if (pos + len > n) return false;
            pos += len;
        }
    };
    while (i < n) {
        const std::uint8_t b = d[i++];
        if (b == 0x3B) return info;                                 // trailer
        if (b == 0x21) {                                            // extension
            if (i >= n) break;
            const std::uint8_t label = d[i++];
            if (label == 0xF9) {                                    // graphic control extension
                if (i + 6 > n || d[i] != 4) return bad("gif graphic control block invalid");
                pending.disposal = static_cast<std::uint8_t>((d[i + 1] >> 2) & 7);
                pending.transparent = (d[i + 1] & 1) != 0;
                pending.rawDelayMs = le16(d + i + 2) * 10u;
                havePending = true;
                i += 6;                                             // size byte + 4 data + terminator
            } else if (label == 0xFF) {                             // application extension
                if (i >= n) break;
                const std::size_t sz = d[i++];
                if (i + sz > n) break;
                const bool netscape = sz == 11 && (std::memcmp(d + i, "NETSCAPE2.0", 11) == 0 || std::memcmp(d + i, "ANIMEXTS1.0", 11) == 0);
                i += sz;
                if (netscape && i + 4 <= n && d[i] == 3 && d[i + 1] == 1) info.loopCount = static_cast<int>(le16(d + i + 2));
                if (!skipSubBlocks(i)) return bad("gif extension truncated");
            } else if (!skipSubBlocks(i)) return bad("gif extension truncated");
            continue;
        }
        if (b == 0x2C) {                                            // image descriptor
            if (i + 9 > n) return bad("gif image descriptor truncated");
            const std::uint8_t packed = d[i + 8];
            i += 9;
            if (packed & 0x80) i += 3u * (std::size_t(1) << ((packed & 7) + 1));
            if (i >= n) return bad("gif image data truncated");
            ++i;                                                    // LZW minimum code size
            if (!skipSubBlocks(i)) return bad("gif image data truncated");
            if (info.frames.size() >= Limits::kMaxGifFrames) { info.frameCapExceeded = true; return info; }
            info.frames.push_back(havePending ? pending : GifFrameInfo{});
            pending = GifFrameInfo{}; havePending = false;
            continue;
        }
        return bad("gif block type invalid");
    }
    if (info.frames.empty()) return bad("gif has no frames");
    return info;       // missing trailer: tolerated, decoders do the same; frames seen so far are valid
}

}  // namespace ss::media
