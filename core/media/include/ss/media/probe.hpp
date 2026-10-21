// StageSignal media: header sniffing and bounded header parsing (REQ-S07, section 13/16).
// Everything here reads at most the bytes the caller passes in and allocates only O(frames) metadata,
// with the frame count capped, so hostile files cannot cause large allocations before validation.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/media/format.hpp"

namespace ss::media {

enum class SniffReject : std::uint8_t { None, TooShort, UnknownSignature, ActiveContent, UnsupportedBrand };

struct SniffResult {
    Format format = Format::Unknown;
    SniffReject reject = SniffReject::None;
    std::string detail;                       // sanitized, never contains file content
    bool ok() const { return format != Format::Unknown; }
};

// Allowlist by magic bytes ONLY (PNG, JPEG, BMP, GIF, MP4, MOV, Matroska). There is deliberately no
// filename/extension parameter. SVG/HTML/script/XML-looking content is reported as ActiveContent.
// Pass at least the first 512 bytes (more is ignored).
SniffResult sniff(const std::uint8_t* data, std::size_t size);
inline SniffResult sniff(const std::string& bytes) {
    return sniff(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

// Declared-vs-sniffed cross check for MediaRef.format. Mismatch is an error (a renamed file never
// changes what a decoder is asked to parse).
Status checkDeclaredFormat(const std::string& declared, const SniffResult& sniffed);

struct ImageHeader { std::uint32_t width = 0, height = 0; };

// PNG / JPEG / BMP dimensions from the header only. Zero dimensions and truncated headers are errors.
Result<ImageHeader> readImageHeader(const std::uint8_t* data, std::size_t size, Format format);

// One GIF frame as declared in the container (no LZW decoding happens here).
struct GifFrameInfo {
    std::uint32_t rawDelayMs = 0;            // as stored (centiseconds * 10); 0 when no graphic control block
    std::uint8_t disposal = 0;               // 0..7 as stored
    bool transparent = false;
};
struct GifInfo {
    std::uint32_t width = 0, height = 0;
    std::vector<GifFrameInfo> frames;
    int loopCount = -1;                      // -1 no NETSCAPE block (play once), 0 infinite, n = n extra plays
    bool frameCapExceeded = false;           // true when the walk stopped at kMaxGifFrames + 1
};

// Walks GIF blocks (header, screen descriptor, extensions, image descriptors) with explicit bounds.
// Stops at Limits::kMaxGifFrames frames and sets frameCapExceeded when another frame follows, so a
// 10^6-frame file costs O(1000) metadata entries and is rejected by validateGif.
Result<GifInfo> parseGif(const std::uint8_t* data, std::size_t size);

}  // namespace ss::media
