// Still-image decoding and text raster (media.md data flow steps 1-2; app-adapters media). Owner: engine-media-record.
// Validation runs on the header numbers first (ss::media), pixels are decoded by juce::ImageFileFormat afterwards.
// GIF: the first frame only (JUCE decodes no animation); video containers are refused with a plain reason.
#pragma once
#include <string>
#include "ss/base/error.hpp"
#include "ss/base/types.hpp"
#include "ss/media/validate.hpp"

namespace ss::app {

struct DecodedImage {
    ss::PixelBuffer pixels;                 // RGBA8 premultiplied
    std::string note;                       // e.g. "Animation not supported: first frame shown", "Reduced to fit"
};

ss::Result<DecodedImage> decodeImageFile(const std::string& utf8Path, ss::media::Tier tier = ss::media::Tier::Standard);
ss::Result<DecodedImage> decodeImageBytes(const std::string& bytes, ss::media::Tier tier = ss::media::Tier::Standard);

// Centred, word-wrapped text on a transparent canvas; premultiplied RGBA8. Text is validated (UTF-8, <= 4096 code points).
ss::Result<ss::PixelBuffer> rasterText(const std::string& utf8, std::uint32_t width, std::uint32_t height,
                                       std::uint8_t r = 255, std::uint8_t g = 255, std::uint8_t b = 255, bool shadowBackdrop = true);

}  // namespace ss::app
