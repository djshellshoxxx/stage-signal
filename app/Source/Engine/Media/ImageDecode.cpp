#include "Engine/Media/ImageDecode.h"
#include <algorithm>
#include <juce_graphics/juce_graphics.h>
#include "ss/media/probe.hpp"

namespace ss::app {
namespace {
using ss::ErrorCategory;
ss::Error bad(const std::string& d) { return ss::makeError(ErrorCategory::DecodeFailure, d, ss::Recoverability::OperatorAction); }

ss::PixelBuffer toPixelBuffer(const juce::Image& src) {
    juce::Image img = src.convertedToFormat(juce::Image::ARGB);
    ss::PixelBuffer pb(static_cast<std::uint32_t>(img.getWidth()), static_cast<std::uint32_t>(img.getHeight()));
    juce::Image::BitmapData bd(img, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < img.getHeight(); ++y) {
        auto* line = bd.getLinePointer(y);
        std::uint8_t* dst = pb.row(static_cast<std::uint32_t>(y));
        for (int x = 0; x < img.getWidth(); ++x) {
            const auto* px = reinterpret_cast<const juce::PixelARGB*>(line + x * bd.pixelStride);   // stored premultiplied
            dst[x * 4 + 0] = px->getRed(); dst[x * 4 + 1] = px->getGreen(); dst[x * 4 + 2] = px->getBlue(); dst[x * 4 + 3] = px->getAlpha();
        }
    }
    return pb;
}
}  // namespace

ss::Result<DecodedImage> decodeImageBytes(const std::string& bytes, ss::media::Tier tier) {
    using ss::media::Format;
    const auto sn = ss::media::sniff(bytes);
    if (!sn.ok()) return bad("This file type is not accepted (PNG, JPEG, BMP or GIF only)");
    if (ss::media::isVideoContainer(sn.format)) return bad("Video files are not supported in this build");
    DecodedImage out;
    std::uint32_t w = 0, h = 0;
    if (sn.format == Format::Gif) {
        auto g = ss::media::parseGif(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        if (!g.ok()) return bad("The GIF header is not valid");
        w = g.value().width; h = g.value().height;
        out.note = "Animation is not supported yet: the first frame is shown";
    } else {
        auto hd = ss::media::readImageHeader(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), sn.format);
        if (!hd.ok()) return bad("The image header is not valid");
        w = hd.value().width; h = hd.value().height;
    }
    auto plan = ss::media::validateStill(w, h, tier);
    if (!plan.ok()) return bad(plan.error().detail);
    juce::MemoryInputStream in(bytes.data(), bytes.size(), false);
    juce::Image img = juce::ImageFileFormat::loadFrom(in);
    if (!img.isValid()) return bad("The image could not be decoded");
    if (sn.format != Format::Gif && (static_cast<std::uint32_t>(img.getWidth()) != w || static_cast<std::uint32_t>(img.getHeight()) != h))
        return bad("The image size does not match its header");
    if (plan.value().needsDownscale) {
        img = img.rescaled(static_cast<int>(plan.value().targetWidth), static_cast<int>(plan.value().targetHeight), juce::Graphics::highResamplingQuality);
        out.note += (out.note.empty() ? "" : "; ") + std::string("Reduced to fit this machine's media budget");
    }
    out.pixels = toPixelBuffer(img);
    return out;
}

ss::Result<DecodedImage> decodeImageFile(const std::string& utf8Path, ss::media::Tier tier) {
    juce::File f(juce::String::fromUTF8(utf8Path.c_str()));
    if (!f.existsAsFile()) return bad("The file does not exist");
    if (f.getSize() > (64ll << 20)) return bad("The file is larger than 64 MiB");
    juce::MemoryBlock mb;
    if (!f.loadFileAsData(mb)) return bad("The file could not be read");
    return decodeImageBytes(std::string(static_cast<const char*>(mb.getData()), mb.getSize()), tier);
}

ss::Result<ss::PixelBuffer> rasterText(const std::string& utf8, std::uint32_t width, std::uint32_t height,
                                       std::uint8_t r, std::uint8_t g, std::uint8_t b, bool backdrop) {
    auto v = ss::media::validateText(utf8);
    if (!v.ok()) return v.error();
    if (width < 16 || height < 16 || width > 4096 || height > 4096)
        return ss::makeError(ErrorCategory::InvalidArgument, "Text area must be between 16 and 4096 pixels");
    juce::Image img(juce::Image::ARGB, static_cast<int>(width), static_cast<int>(height), true);
    {
        juce::Graphics gr(img);
        if (backdrop) { gr.setColour(juce::Colour::fromRGBA(0, 0, 0, 140)); gr.fillRoundedRectangle(img.getBounds().toFloat(), static_cast<float>(height) * 0.12f); }
        gr.setColour(juce::Colour::fromRGB(r, g, b));
        gr.setFont(juce::FontOptions(std::max(10.0f, static_cast<float>(height) * 0.38f), juce::Font::bold));
        gr.drawFittedText(juce::String::fromUTF8(utf8.c_str()), img.getBounds().reduced(static_cast<int>(width / 40)), juce::Justification::centred, 4, 0.8f);
    }
    return toPixelBuffer(img);
}

}  // namespace ss::app
