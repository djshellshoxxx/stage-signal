// MOD-07 import gate (spec §13 Beta imports/Limits, §16 import security). Pure functions, no threads, no globals.
// Every entry point is exception-safe: malformed input yields a Status, never a crash or escaped exception.
// Dimensions and byte counts are header-probed and checked BEFORE any pixel buffer is allocated (REQ-S07).
#pragma once
#include "core/types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ss { namespace media {

enum class Tier : uint8_t { Base, Standard };   // Standard stands for STANDARD/ENHANCED/SHOW (higher-tier limits)

struct ImportLimits {
  int maxSrcW = 0, maxSrcH = 0;       // source still/GIF-screen dimension cap (checked from the header)
  uint64_t maxSrcDecodedBytes = 0;    // w*h*4 cap for a single still
  uint64_t maxFileBytes = 0;          // encoded file size cap
  int outMaxW = 0, outMaxH = 0;       // >0: stills are box-downscaled to fit (BASE preprocessing, 800x600)
  uint32_t maxGifFrames = 1000;
  uint64_t gifDecodedBudget = 0;      // w*h*4*frames cap for one GIF (== tier cache budget)
};
// BASE: source <=2048x2048 and <=16 MiB decoded (our transient-memory choice for the 96 MiB process budget),
//       output <=800x600, GIF decoded <=8 MiB. Standard: <=4096x4096, <=64 MiB/image, GIF <=64 MiB.
ImportLimits limitsFor(Tier t);
constexpr uint64_t kBaseCacheBudgetBytes = 8ull << 20;       // spec §13: combined decoded source cache (BASE)
constexpr uint64_t kStandardCacheBudgetBytes = 64ull << 20;

enum class AssetKind : uint8_t { Still, Gif, Video };
enum class Container : uint8_t { Unknown, Png, Jpeg, Bmp, Gif, Mp4Mov, Matroska, Markup /*svg/html/script/xml*/ };

// Local paths only: rejects empty/NUL/control chars, URLs and any "scheme:" prefix (file:, http:, data:...), UNC and
// device paths (\\host, //host, \\?\, \\.\), ".." components, alternate data streams and drive-relative "C:foo".
// Drive-absolute "C:\x" and POSIX absolute paths are allowed (user-chosen files); archives must use relative-only checks.
Status validateLocalPath(const std::string& path);
// Allowed extensions: png jpg jpeg bmp (Still), gif (Gif), mp4 mov mkv (Video). svg/html/htm/js/xml/... are rejected.
Status classifyExtension(const std::string& path, AssetKind& kind);
// Magic-byte sniff of the container (first bytes); Markup when content starts with '<' (after BOM/whitespace).
Container sniffContainer(const uint8_t* d, size_t n);
// Extension AND magic must agree (png<->Png, jpg/jpeg<->Jpeg, bmp<->Bmp, gif<->Gif, mp4/mov<->Mp4Mov, mkv<->Matroska).
Status checkExtensionAndMagic(const std::string& path, const uint8_t* head, size_t n, AssetKind& kind);

struct ImageProbe { Container fmt = Container::Unknown; int w = 0, h = 0; uint64_t decodedBytes = 0; };
// Header-only dimension probe for PNG/JPEG/BMP/GIF (allocates nothing). Fails on truncated/zero-dim/overflowing headers.
Status probeImage(const uint8_t* d, size_t n, ImageProbe& out);

struct DecodedStill {
  int w = 0, h = 0;                   // output size (after BASE preprocessing)
  int srcW = 0, srcH = 0;
  bool hasAlpha = false, downscaled = false;
  std::vector<uint32_t> argb;         // 0xAARRGGBB straight alpha, row-major, stride == w
  std::vector<std::string> warnings;
  uint64_t bytes() const { return (uint64_t)argb.size() * 4u; }
};
// `name` is only used for the extension check (may be "" for in-memory data, then magic bytes alone decide).
Status validateAndDecodeStill(const uint8_t* data, size_t n, const std::string& name, const ImportLimits& lim, DecodedStill& out);
Status validateAndDecodeStillFile(const std::string& path, const ImportLimits& lim, DecodedStill& out);

struct DecodedGif {
  int w = 0, h = 0;
  uint32_t frameCount = 0;
  std::vector<uint32_t> argb;         // frameCount * w*h, fully composited frames (disposal + transparency applied)
  std::vector<int> delayMs;           // per frame, clamped to >=20 ms
  std::vector<int> rawDelayMs;        // per frame as authored
  int loopCount = -1;                 // NETSCAPE2.0: 0 infinite, n repeats, -1 extension absent (play once)
  bool hasTransparency = false;
  std::vector<std::string> warnings;
  uint64_t bytes() const { return (uint64_t)argb.size() * 4u; }
  int64_t totalMs() const { int64_t t = 0; for (int d : delayMs) t += d; return t; }
};
constexpr int kMinGifDelayMs = 20;
Status validateAndDecodeGif(const uint8_t* data, size_t n, const std::string& name, const ImportLimits& lim, DecodedGif& out);
Status validateAndDecodeGifFile(const std::string& path, const ImportLimits& lim, DecodedGif& out);

// ---- helpers shared with manager/manifest -------------------------------------------------------------------
// Bounded file read: size is checked (fseek/ftell) BEFORE allocating; UTF-8 path (wide API on Windows).
// These do NOT call validateLocalPath (tooling passes trusted dirs); every media-facing caller validates first.
Status readFileBounded(const std::string& path, uint64_t maxBytes, std::vector<uint8_t>& out, uint64_t* fileSize = nullptr);
Status readFileHead(const std::string& path, size_t maxBytes, std::vector<uint8_t>& out, uint64_t* fileSize = nullptr);
std::string baseName(const std::string& path);                 // for sanitized error text (no directories/usernames)
// Flatten straight-alpha ARGB onto an opaque matte (IF-07 Frame has no alpha channel). Result is 0x00RRGGBB.
void flattenArgb(const uint32_t* argb, size_t count, uint32_t matteRgb, uint32_t* outRgb);
Frame frameFromArgb(const uint32_t* argb, int w, int h, uint32_t matteRgb);
// Box-filter downscale (premultiplied accumulation), integer math only.
void downscaleBoxArgb(const uint32_t* src, int sw, int sh, int dw, int dh, std::vector<uint32_t>& out);
// Normalized crop rectangle (0..1). Invalid/empty rect -> InvalidArgument. Identity crop is the full frame.
struct CropRect { float x = 0, y = 0, w = 1, h = 1; bool isIdentity() const { return x == 0 && y == 0 && w == 1 && h == 1; } };
Status validateCrop(const CropRect& c);
Frame cropFrame(const Frame& in, const CropRect& c);

}} // namespace ss::media
