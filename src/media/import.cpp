#include "media/import.h"
#include "media/stb_gif.h"
#include "stb/stb_image.h"       // declarations only (implementation lives in stb_impl.cpp)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <new>
#include <limits>

namespace ss { namespace media {

namespace {
inline uint32_t le16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
inline uint32_t le32(const uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }
inline uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
inline uint32_t be32(const uint8_t* p) { return (be16(p) << 16) | be16(p + 2); }
inline std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }
inline bool isSep(char c) { return c == '/' || c == '\\'; }

std::string extOf(const std::string& path) {
  std::string b = baseName(path);
  size_t dot = b.rfind('.');
  if (dot == std::string::npos || dot + 1 >= b.size()) return "";
  return lower(b.substr(dot + 1));
}
const char* containerName(Container c) {
  switch (c) { case Container::Png: return "PNG"; case Container::Jpeg: return "JPEG"; case Container::Bmp: return "BMP";
    case Container::Gif: return "GIF"; case Container::Mp4Mov: return "MP4/MOV"; case Container::Matroska: return "Matroska";
    case Container::Markup: return "markup"; default: return "unknown"; }
}
}  // namespace

ImportLimits limitsFor(Tier t) {
  ImportLimits l;
  if (t == Tier::Base) {
    l.maxSrcW = 2048; l.maxSrcH = 2048; l.maxSrcDecodedBytes = 16ull << 20; l.maxFileBytes = 16ull << 20;
    l.outMaxW = 800; l.outMaxH = 600; l.gifDecodedBudget = kBaseCacheBudgetBytes;
  } else {
    l.maxSrcW = 4096; l.maxSrcH = 4096; l.maxSrcDecodedBytes = 64ull << 20; l.maxFileBytes = 64ull << 20;
    l.outMaxW = 0; l.outMaxH = 0; l.gifDecodedBudget = kStandardCacheBudgetBytes;
  }
  l.maxGifFrames = 1000;
  return l;
}

std::string baseName(const std::string& path) {
  size_t p = path.find_last_of("/\\");
  return p == std::string::npos ? path : path.substr(p + 1);
}

// ---- path / type gating ---------------------------------------------------------------------------------------
Status validateLocalPath(const std::string& path) {
  auto bad = [](const char* why) { return Status::Fail(Err::InvalidArgument, std::string("path rejected: ") + why, true); };
  if (path.empty()) return bad("empty");
  if (path.size() > 4096) return bad("too long");
  for (unsigned char c : path) if (c < 0x20 || c == 0x7F) return bad("control character");
  if (path.size() >= 2 && isSep(path[0]) && isSep(path[1])) return bad("UNC/device path");
  size_t colon = path.find(':');
  if (colon != std::string::npos) {
    bool drive = colon == 1 && ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z')) &&
                 path.size() > 2 && isSep(path[2]) && path.find(':', 2) == std::string::npos;
    if (!drive) return bad("URL, scheme, stream or drive-relative form");
  }
  size_t i = 0;
  while (i <= path.size()) {
    size_t j = i;
    while (j < path.size() && !isSep(path[j])) ++j;
    std::string comp = path.substr(i, j - i);
    if (comp == "..") return bad("'..' traversal");
    std::string stem = lower(comp.substr(0, comp.find('.')));
    static const char* kDev[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "lpt1", "lpt2", "lpt3"};
    for (const char* d : kDev) if (stem == d) return bad("reserved device name");
    i = j + 1;
  }
  return Status::Ok();
}

Status classifyExtension(const std::string& path, AssetKind& kind) {
  std::string e = extOf(path);
  if (e == "png" || e == "jpg" || e == "jpeg" || e == "bmp") { kind = AssetKind::Still; return Status::Ok(); }
  if (e == "gif") { kind = AssetKind::Gif; return Status::Ok(); }
  if (e == "mp4" || e == "mov" || e == "mkv") { kind = AssetKind::Video; return Status::Ok(); }
  if (e == "svg" || e == "svgz" || e == "html" || e == "htm" || e == "xhtml" || e == "js" || e == "xml" || e == "php" || e == "hta")
    return Status::Fail(Err::InvalidArgument, "SVG/HTML/script content is not an allowed import: ." + e);
  return Status::Fail(Err::InvalidArgument, "unsupported media extension: ." + e);
}

Container sniffContainer(const uint8_t* d, size_t n) {
  static const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (n >= 8 && !std::memcmp(d, png, 8)) return Container::Png;
  if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return Container::Jpeg;
  if (n >= 2 && d[0] == 'B' && d[1] == 'M') return Container::Bmp;
  if (n >= 6 && (!std::memcmp(d, "GIF87a", 6) || !std::memcmp(d, "GIF89a", 6))) return Container::Gif;
  if (n >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3) return Container::Matroska;
  if (n >= 12) {
    static const char* kAtoms[] = {"ftyp", "moov", "mdat", "wide", "free", "skip", "pnot"};
    for (const char* a : kAtoms) if (!std::memcmp(d + 4, a, 4)) return Container::Mp4Mov;
  }
  size_t i = 0;
  if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) i = 3;
  else if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) i = 2;
  while (i < n && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n' || d[i] == 0)) ++i;
  if (i < n && d[i] == '<') return Container::Markup;
  return Container::Unknown;
}

Status checkExtensionAndMagic(const std::string& path, const uint8_t* head, size_t n, AssetKind& kind) {
  Status s = classifyExtension(path, kind);
  if (!s.ok()) return s;
  Container c = sniffContainer(head, n);
  if (c == Container::Markup) return Status::Fail(Err::InvalidArgument, "SVG/HTML/script content is not an allowed import: " + baseName(path));
  Container want = Container::Unknown;
  std::string e = extOf(path);
  if (e == "png") want = Container::Png; else if (e == "jpg" || e == "jpeg") want = Container::Jpeg;
  else if (e == "bmp") want = Container::Bmp; else if (e == "gif") want = Container::Gif;
  else if (e == "mp4" || e == "mov") want = Container::Mp4Mov; else if (e == "mkv") want = Container::Matroska;
  if (c != want)
    return Status::Fail(Err::InvalidArgument, std::string("content (") + containerName(c) + ") does not match extension ." + e + ": " + baseName(path));
  return Status::Ok();
}

// ---- header probes (no allocation) -----------------------------------------------------------------------------
static Status probeFail(const char* why) { return Status::Fail(Err::DecodeFailure, std::string("image header: ") + why, true); }

static Status probePng(const uint8_t* d, size_t n, ImageProbe& o) {
  if (n < 33) return probeFail("truncated PNG");
  if (be32(d + 8) != 13 || std::memcmp(d + 12, "IHDR", 4)) return probeFail("PNG IHDR missing");
  uint32_t w = be32(d + 16), h = be32(d + 20);
  if (w == 0 || h == 0) return probeFail("zero dimension");
  if (w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) return probeFail("dimension overflow");
  // Walk chunks to confirm the stream is complete (IEND present) - catches truncation stb might tolerate.
  size_t pos = 8; bool iend = false; bool idat = false;
  while (pos + 12 <= n) {
    uint32_t len = be32(d + pos);
    const uint8_t* type = d + pos + 4;
    if ((uint64_t)pos + 12 + len > n) return probeFail("truncated PNG chunk");
    if (!std::memcmp(type, "IDAT", 4)) idat = true;
    if (!std::memcmp(type, "IEND", 4)) { iend = true; break; }
    pos += 12 + (size_t)len;
  }
  if (!iend || !idat) return probeFail("truncated PNG (no IDAT/IEND)");
  o.fmt = Container::Png; o.w = (int)w; o.h = (int)h; return Status::Ok();
}

static Status probeJpeg(const uint8_t* d, size_t n, ImageProbe& o) {
  size_t i = 2; bool found = false; uint32_t w = 0, h = 0;
  while (i + 4 <= n && !found) {
    if (d[i] != 0xFF) { ++i; continue; }
    while (i < n && d[i] == 0xFF) ++i;
    if (i >= n) break;
    uint8_t m = d[i++];
    if (m == 0 || m == 1 || m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) continue;
    if (m == 0xD9 || m == 0xDA) return probeFail("JPEG has no frame header");
    if (i + 2 > n) break;
    uint32_t len = be16(d + i);
    if (len < 2) return probeFail("bad JPEG segment");
    bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
    if (sof) {
      if (len < 8 || i + 8 > n) return probeFail("truncated JPEG SOF");
      h = be16(d + i + 3); w = be16(d + i + 5);
      found = true;
    } else i += len;
  }
  if (!found) return probeFail("truncated JPEG");
  if (w == 0 || h == 0) return probeFail("zero dimension");
  // Require an EOI marker near the end: stb would otherwise decode a truncated JPEG as a partial image.
  bool eoi = false; size_t lo = n > 4096 ? n - 4096 : 0;
  for (size_t k = n; k > lo + 1; --k) if (d[k - 2] == 0xFF && d[k - 1] == 0xD9) { eoi = true; break; }
  if (!eoi) return probeFail("truncated JPEG (no EOI)");
  o.fmt = Container::Jpeg; o.w = (int)w; o.h = (int)h; return Status::Ok();
}

static Status probeBmp(const uint8_t* d, size_t n, ImageProbe& o) {
  if (n < 26) return probeFail("truncated BMP");
  uint32_t dib = le32(d + 14), offset = le32(d + 10);
  int64_t w, h; uint32_t bpp = 0, comp = 0;
  if (dib == 12) { w = (int16_t)le16(d + 18); h = (int16_t)le16(d + 20); bpp = le16(d + 24); }
  else if (dib >= 40 && n >= 54) { w = (int32_t)le32(d + 18); h = (int32_t)le32(d + 22); bpp = le16(d + 28); comp = le32(d + 30); }
  else return probeFail("unsupported or truncated BMP header");
  if (w <= 0 || h == 0) return probeFail("zero/negative dimension");
  if (h < 0) h = -h;
  if (w > 0x7FFFFFFF || h > 0x7FFFFFFF) return probeFail("dimension overflow");
  if (bpp == 0 || bpp > 32) return probeFail("bad BMP bit depth");
  if (comp == 0 || comp == 3) {
    uint64_t row = (((uint64_t)w * bpp + 31) / 32) * 4;
    if ((uint64_t)offset + row * (uint64_t)h > n) return probeFail("truncated BMP pixel data");
  }
  o.fmt = Container::Bmp; o.w = (int)w; o.h = (int)h; return Status::Ok();
}

namespace {
struct GifScan {
  int w = 0, h = 0; uint32_t frames = 0; std::vector<int> delayCs; int loop = -1; bool transparent = false;
};
Status skipSubBlocks(const uint8_t* d, size_t n, size_t& pos) {
  for (;;) {
    if (pos >= n) return probeFail("truncated GIF");
    uint8_t len = d[pos++];
    if (len == 0) return Status::Ok();
    if ((uint64_t)pos + len > n) return probeFail("truncated GIF");
    pos += len;
  }
}
// Walks the block structure with bounds checks; counts frames and stops at maxFrames+1 (before any pixel allocation).
Status scanGif(const uint8_t* d, size_t n, uint32_t maxFrames, GifScan& s) {
  if (n < 14) return probeFail("truncated GIF");
  s.w = (int)le16(d + 6); s.h = (int)le16(d + 8);
  if (s.w == 0 || s.h == 0) return probeFail("zero dimension");
  uint8_t flags = d[10];
  size_t pos = 13;
  if (flags & 0x80) pos += 3u * (2u << (flags & 7));
  if (pos > n) return probeFail("truncated GIF color table");
  int pendingCs = -1; bool sawTrailer = false;
  while (!sawTrailer) {
    if (pos >= n) return probeFail("truncated GIF (no trailer)");
    uint8_t tag = d[pos++];
    if (tag == 0x3B) { sawTrailer = true; break; }
    if (tag == 0x21) {
      if (pos >= n) return probeFail("truncated GIF");
      uint8_t label = d[pos++];
      if (label == 0xF9 && pos + 6 <= n && d[pos] == 4) {
        pendingCs = (int)le16(d + pos + 2);
        if (d[pos + 1] & 1) s.transparent = true;
        pos += 5;
        if (pos >= n) return probeFail("truncated GIF");
        if (d[pos] == 0) { ++pos; continue; }
        // malformed terminator: fall through to generic skip
      } else if (label == 0xFF && pos < n && d[pos] == 11 && pos + 12 <= n &&
                 (!std::memcmp(d + pos + 1, "NETSCAPE2.0", 11) || !std::memcmp(d + pos + 1, "ANIMEXTS1.0", 11))) {
        pos += 12;
        if (pos + 5 <= n && d[pos] >= 3 && d[pos + 1] == 1) s.loop = (int)le16(d + pos + 2);
      }
      // any other extension (comment, plain text, application): skip its length-prefixed sub-blocks
      Status st = skipSubBlocks(d, n, pos);
      if (!st.ok()) return st;
      continue;
    }
    if (tag == 0x2C) {
      if (pos + 9 > n) return probeFail("truncated GIF image descriptor");
      uint32_t x = le16(d + pos), y = le16(d + pos + 2), w = le16(d + pos + 4), h = le16(d + pos + 6);
      uint8_t lf = d[pos + 8];
      pos += 9;
      if (x + w > (uint32_t)s.w || y + h > (uint32_t)s.h) return probeFail("GIF frame outside logical screen");
      if (lf & 0x80) pos += 3u * (2u << (lf & 7));
      if (pos >= n) return probeFail("truncated GIF");
      uint8_t minCode = d[pos++];
      if (minCode < 1 || minCode > 8) return probeFail("bad GIF LZW code size");
      Status st = skipSubBlocks(d, n, pos);
      if (!st.ok()) return st;
      if (s.frames >= maxFrames) return Status::Fail(Err::ResourceExhausted, "GIF frame count exceeds limit", true);
      ++s.frames; s.delayCs.push_back(pendingCs < 0 ? 0 : pendingCs); pendingCs = -1;
      continue;
    }
    return probeFail("corrupt GIF block");
  }
  if (s.frames == 0) return probeFail("GIF has no frames");
  return Status::Ok();
}
}  // namespace

Status probeImage(const uint8_t* d, size_t n, ImageProbe& out) {
  out = ImageProbe();
  Container c = sniffContainer(d, n);
  Status s;
  switch (c) {
    case Container::Png: s = probePng(d, n, out); break;
    case Container::Jpeg: s = probeJpeg(d, n, out); break;
    case Container::Bmp: s = probeBmp(d, n, out); break;
    case Container::Gif: {
      if (n < 10) return probeFail("truncated GIF");
      GifScan g; s = scanGif(d, n, 1000000u, g);
      if (s.ok()) { out.fmt = Container::Gif; out.w = g.w; out.h = g.h; }
      break;
    }
    case Container::Markup: return Status::Fail(Err::InvalidArgument, "SVG/HTML/script content is not an allowed import");
    default: return Status::Fail(Err::InvalidArgument, std::string("unrecognized image data (") + containerName(c) + ")");
  }
  if (!s.ok()) return s;
  out.decodedBytes = (uint64_t)out.w * (uint64_t)out.h * 4u;
  return Status::Ok();
}

// ---- pixel helpers ---------------------------------------------------------------------------------------------
void flattenArgb(const uint32_t* argb, size_t count, uint32_t matte, uint32_t* out) {
  uint32_t mr = (matte >> 16) & 255, mg = (matte >> 8) & 255, mb = matte & 255;
  for (size_t i = 0; i < count; ++i) {
    uint32_t p = argb[i], a = p >> 24;
    if (a == 255) { out[i] = p & 0x00FFFFFFu; continue; }
    if (a == 0) { out[i] = matte & 0x00FFFFFFu; continue; }
    uint32_t r = (((p >> 16) & 255) * a + mr * (255 - a) + 127) / 255;
    uint32_t g = (((p >> 8) & 255) * a + mg * (255 - a) + 127) / 255;
    uint32_t b = ((p & 255) * a + mb * (255 - a) + 127) / 255;
    out[i] = (r << 16) | (g << 8) | b;
  }
}
Frame frameFromArgb(const uint32_t* argb, int w, int h, uint32_t matte) {
  Frame f; f.w = w; f.h = h; f.px.resize((size_t)w * (size_t)h);
  flattenArgb(argb, f.px.size(), matte, f.px.data());
  return f;
}

void downscaleBoxArgb(const uint32_t* src, int sw, int sh, int dw, int dh, std::vector<uint32_t>& out) {
  out.assign((size_t)dw * (size_t)dh, 0u);
  for (int dy = 0; dy < dh; ++dy) {
    int sy0 = (int)((int64_t)dy * sh / dh), sy1 = (int)((int64_t)(dy + 1) * sh / dh);
    if (sy1 <= sy0) sy1 = sy0 + 1;
    for (int dx = 0; dx < dw; ++dx) {
      int sx0 = (int)((int64_t)dx * sw / dw), sx1 = (int)((int64_t)(dx + 1) * sw / dw);
      if (sx1 <= sx0) sx1 = sx0 + 1;
      uint64_t sa = 0, sr = 0, sg = 0, sb = 0, cnt = 0;
      for (int y = sy0; y < sy1; ++y) {
        const uint32_t* row = src + (size_t)y * sw;
        for (int x = sx0; x < sx1; ++x) {
          uint32_t p = row[x], a = p >> 24;
          sa += a; sr += (uint64_t)a * ((p >> 16) & 255); sg += (uint64_t)a * ((p >> 8) & 255); sb += (uint64_t)a * (p & 255); ++cnt;
        }
      }
      if (sa == 0) { out[(size_t)dy * dw + dx] = 0; continue; }
      uint32_t a = (uint32_t)((sa + cnt / 2) / cnt), r = (uint32_t)(sr / sa), g = (uint32_t)(sg / sa), b = (uint32_t)(sb / sa);
      out[(size_t)dy * dw + dx] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
}

Status validateCrop(const CropRect& c) {
  bool fin = std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.w) && std::isfinite(c.h);
  if (!fin || c.x < 0 || c.y < 0 || c.w <= 0 || c.h <= 0 || c.x >= 1 || c.y >= 1 || c.x + c.w > 1.0001f || c.y + c.h > 1.0001f)
    return Status::Fail(Err::InvalidArgument, "crop rectangle must be a non-empty normalized rect inside 0..1");
  return Status::Ok();
}
Frame cropFrame(const Frame& in, const CropRect& c) {
  if (c.isIdentity() || in.w <= 0 || in.h <= 0) return in;
  int x0 = (int)std::floor(c.x * in.w + 0.5f), y0 = (int)std::floor(c.y * in.h + 0.5f);
  int cw = (int)std::floor(c.w * in.w + 0.5f), ch = (int)std::floor(c.h * in.h + 0.5f);
  if (x0 >= in.w) x0 = in.w - 1;
  if (y0 >= in.h) y0 = in.h - 1;
  if (cw < 1) cw = 1;
  if (ch < 1) ch = 1;
  if (x0 + cw > in.w) cw = in.w - x0;
  if (y0 + ch > in.h) ch = in.h - y0;
  Frame o; o.resize(cw, ch); o.pts = in.pts; o.generation = in.generation;
  for (int y = 0; y < ch; ++y) std::memcpy(&o.px[(size_t)y * cw], &in.px[(size_t)(y + y0) * in.w + x0], (size_t)cw * 4u);
  return o;
}

// ---- file IO -----------------------------------------------------------------------------------------------------
namespace {
#ifdef _WIN32
bool utf8ToWide(const std::string& s, std::wstring& w) {
  w.clear();
  for (size_t i = 0; i < s.size();) {
    uint32_t c = (unsigned char)s[i]; int extra = 0;
    if (c < 0x80) extra = 0; else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; } else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; } else return false;
    if (i + extra >= s.size() + (extra ? 0 : 1) && extra) return false;
    for (int k = 1; k <= extra; ++k) { if (i + k >= s.size() || ((unsigned char)s[i + k] & 0xC0) != 0x80) return false; c = (c << 6) | ((unsigned char)s[i + k] & 0x3F); }
    i += 1 + extra;
    if (c >= 0x10000) { c -= 0x10000; w.push_back((wchar_t)(0xD800 + (c >> 10))); w.push_back((wchar_t)(0xDC00 + (c & 0x3FF))); } else w.push_back((wchar_t)c);
  }
  return true;
}
#endif
FILE* openRead(const std::string& path) {
#ifdef _WIN32
  std::wstring w; if (!utf8ToWide(path, w)) return nullptr;
  return _wfopen(w.c_str(), L"rb");
#else
  return std::fopen(path.c_str(), "rb");
#endif
}
}  // namespace

Status readFileHead(const std::string& path, size_t maxBytes, std::vector<uint8_t>& out, uint64_t* fileSize) {
  out.clear();
  FILE* f = openRead(path);
  if (!f) return Status::Fail(Err::InvalidArgument, "cannot open media file: " + baseName(path));
  struct Closer { FILE* f; ~Closer() { std::fclose(f); } } closer{f};
  if (std::fseek(f, 0, SEEK_END) != 0) return Status::Fail(Err::DecodeFailure, "cannot size media file: " + baseName(path));
  long sz = std::ftell(f);
  if (sz < 0) return Status::Fail(Err::ResourceExhausted, "media file too large or unsizable: " + baseName(path));
  if (fileSize) *fileSize = (uint64_t)sz;
  std::fseek(f, 0, SEEK_SET);
  size_t want = (uint64_t)sz < maxBytes ? (size_t)sz : maxBytes;
  try { out.resize(want); } catch (const std::bad_alloc&) { return Status::Fail(Err::ResourceExhausted, "out of memory reading media"); }
  size_t got = want ? std::fread(out.data(), 1, want, f) : 0;
  if (got != want) { out.clear(); return Status::Fail(Err::DecodeFailure, "short read: " + baseName(path)); }
  return Status::Ok();
}
Status readFileBounded(const std::string& path, uint64_t maxBytes, std::vector<uint8_t>& out, uint64_t* fileSize) {
  out.clear();
  FILE* f = openRead(path);
  if (!f) return Status::Fail(Err::InvalidArgument, "cannot open media file: " + baseName(path));
  long sz = -1;
  if (std::fseek(f, 0, SEEK_END) == 0) sz = std::ftell(f);
  std::fclose(f);
  if (sz < 0) return Status::Fail(Err::ResourceExhausted, "media file too large or unsizable: " + baseName(path));
  if (fileSize) *fileSize = (uint64_t)sz;
  if ((uint64_t)sz > maxBytes) return Status::Fail(Err::ResourceExhausted, "media file exceeds byte limit: " + baseName(path));
  return readFileHead(path, (size_t)sz, out, nullptr);
}

// ---- stills ------------------------------------------------------------------------------------------------------
static Status checkStillLimits(const ImageProbe& p, size_t n, const ImportLimits& lim) {
  if ((uint64_t)n > lim.maxFileBytes) return Status::Fail(Err::ResourceExhausted, "file exceeds byte limit");
  if (p.w > lim.maxSrcW || p.h > lim.maxSrcH)
    return Status::Fail(Err::ResourceExhausted, "image dimensions exceed limit " + std::to_string(lim.maxSrcW) + "x" + std::to_string(lim.maxSrcH));
  if (p.decodedBytes > lim.maxSrcDecodedBytes) return Status::Fail(Err::ResourceExhausted, "decoded size exceeds byte limit");
  return Status::Ok();
}

Status validateAndDecodeStill(const uint8_t* data, size_t n, const std::string& name, const ImportLimits& lim, DecodedStill& out) {
  out = DecodedStill();
  try {
    if (!data || n == 0) return Status::Fail(Err::DecodeFailure, "empty image data");
    if (n > (size_t)std::numeric_limits<int>::max()) return Status::Fail(Err::ResourceExhausted, "file exceeds byte limit");
    if (!name.empty()) {
      AssetKind k; Status s = checkExtensionAndMagic(name, data, n, k); if (!s.ok()) return s;
      if (k != AssetKind::Still) return Status::Fail(Err::InvalidArgument, "not a still-image extension: " + baseName(name));
    }
    ImageProbe p; Status s = probeImage(data, n, p); if (!s.ok()) return s;
    if (p.fmt != Container::Png && p.fmt != Container::Jpeg && p.fmt != Container::Bmp)
      return Status::Fail(Err::InvalidArgument, "not a PNG/JPEG/BMP still");
    s = checkStillLimits(p, n, lim); if (!s.ok()) return s;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(data, (int)n, &w, &h, &comp, 4);
    if (!px) return Status::Fail(Err::DecodeFailure, std::string("decode failed: ") + (stbi_failure_reason() ? stbi_failure_reason() : "unknown"));
    struct Free { unsigned char* p; ~Free() { stbi_image_free(p); } } guard{px};
    if (w != p.w || h != p.h) return Status::Fail(Err::DecodeFailure, "decoded size disagrees with header");
    std::vector<uint32_t> argb((size_t)w * (size_t)h);
    bool alpha = false;
    for (size_t i = 0; i < argb.size(); ++i) {
      const unsigned char* q = px + i * 4;
      uint32_t a = q[3]; if (a != 255) alpha = true;
      argb[i] = a == 0 ? 0u : ((a << 24) | ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2]);
    }
    out.srcW = w; out.srcH = h; out.hasAlpha = alpha;
    if (lim.outMaxW > 0 && lim.outMaxH > 0 && (w > lim.outMaxW || h > lim.outMaxH)) {
      int dw, dh;
      if ((int64_t)w * lim.outMaxH >= (int64_t)h * lim.outMaxW) { dw = lim.outMaxW; dh = (int)((int64_t)h * lim.outMaxW / w); }
      else { dh = lim.outMaxH; dw = (int)((int64_t)w * lim.outMaxH / h); }
      if (dw < 1) dw = 1;
      if (dh < 1) dh = 1;
      downscaleBoxArgb(argb.data(), w, h, dw, dh, out.argb);
      out.w = dw; out.h = dh; out.downscaled = true;
      out.warnings.push_back("image " + std::to_string(w) + "x" + std::to_string(h) + " preprocessed to " + std::to_string(dw) + "x" + std::to_string(dh));
    } else { out.argb.swap(argb); out.w = w; out.h = h; }
    return Status::Ok();
  } catch (const std::bad_alloc&) {
    out = DecodedStill(); return Status::Fail(Err::ResourceExhausted, "out of memory during image decode");
  } catch (...) {
    out = DecodedStill(); return Status::Fail(Err::DecodeFailure, "internal error during image decode", true);
  }
}

Status validateAndDecodeStillFile(const std::string& path, const ImportLimits& lim, DecodedStill& out) {
  out = DecodedStill();
  Status ps = validateLocalPath(path); if (!ps.ok()) return ps;
  AssetKind k; Status es = classifyExtension(path, k); if (!es.ok()) return es;   // before touching the file
  std::vector<uint8_t> bytes;
  try {
    Status rs = readFileBounded(path, lim.maxFileBytes, bytes); if (!rs.ok()) return rs;
  } catch (const std::bad_alloc&) { return Status::Fail(Err::ResourceExhausted, "out of memory reading media"); }
  return validateAndDecodeStill(bytes.data(), bytes.size(), path, lim, out);
}

// ---- GIF ---------------------------------------------------------------------------------------------------------
Status validateAndDecodeGif(const uint8_t* data, size_t n, const std::string& name, const ImportLimits& lim, DecodedGif& out) {
  out = DecodedGif();
  unsigned char* rgba = nullptr;
  try {
    if (!data || n == 0) return Status::Fail(Err::DecodeFailure, "empty GIF data");
    if (n > (size_t)std::numeric_limits<int>::max() || (uint64_t)n > lim.maxFileBytes) return Status::Fail(Err::ResourceExhausted, "file exceeds byte limit");
    if (!name.empty()) {
      AssetKind k; Status s = checkExtensionAndMagic(name, data, n, k); if (!s.ok()) return s;
      if (k != AssetKind::Gif) return Status::Fail(Err::InvalidArgument, "not a GIF extension: " + baseName(name));
    } else if (sniffContainer(data, n) != Container::Gif) return Status::Fail(Err::InvalidArgument, "not a GIF");
    GifScan sc; Status s = scanGif(data, n, lim.maxGifFrames, sc); if (!s.ok()) return s;
    if (sc.w > lim.maxSrcW || sc.h > lim.maxSrcH) return Status::Fail(Err::ResourceExhausted, "GIF dimensions exceed limit");
    uint64_t frameBytes = (uint64_t)sc.w * (uint64_t)sc.h * 4u;
    if (frameBytes > lim.maxSrcDecodedBytes || frameBytes * sc.frames > lim.gifDecodedBudget)
      return Status::Fail(Err::ResourceExhausted, "GIF decoded size exceeds budget (" + std::to_string(sc.frames) + " frames of " +
                          std::to_string(sc.w) + "x" + std::to_string(sc.h) + ")");
    std::vector<int> stbDelay(sc.frames, 0);
    int w = 0, h = 0;
    rgba = ssGifDecode(data, (int)n, (int)sc.frames, stbDelay.data(), &w, &h);
    if (!rgba) return Status::Fail(Err::DecodeFailure, "GIF decode failed (corrupt or truncated)");
    if (w != sc.w || h != sc.h) { std::free(rgba); return Status::Fail(Err::DecodeFailure, "GIF size disagrees with header"); }
    size_t total = (size_t)w * h * sc.frames;
    out.argb.resize(total);
    for (size_t i = 0; i < total; ++i) {
      const unsigned char* q = rgba + i * 4; uint32_t a = q[3];
      out.argb[i] = a == 0 ? 0u : ((a << 24) | ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2]);
    }
    std::free(rgba); rgba = nullptr;
    out.w = w; out.h = h; out.frameCount = sc.frames; out.loopCount = sc.loop;
    out.hasTransparency = sc.transparent;
    uint32_t clamped = 0, firstClamped = 0;
    for (uint32_t i = 0; i < sc.frames; ++i) {
      int raw = sc.delayCs[i] * 10, d = raw;
      if (d < kMinGifDelayMs) { d = kMinGifDelayMs; if (!clamped) firstClamped = i; ++clamped; }
      out.rawDelayMs.push_back(raw); out.delayMs.push_back(d);
    }
    if (clamped) out.warnings.push_back(std::to_string(clamped) + " GIF frame delay(s) below 20 ms clamped to 20 ms (first at frame " + std::to_string(firstClamped) + ")");
    return Status::Ok();
  } catch (const std::bad_alloc&) {
    std::free(rgba); out = DecodedGif(); return Status::Fail(Err::ResourceExhausted, "out of memory during GIF decode");
  } catch (...) {
    std::free(rgba); out = DecodedGif(); return Status::Fail(Err::DecodeFailure, "internal error during GIF decode", true);
  }
}

Status validateAndDecodeGifFile(const std::string& path, const ImportLimits& lim, DecodedGif& out) {
  out = DecodedGif();
  Status ps = validateLocalPath(path); if (!ps.ok()) return ps;
  AssetKind k; Status es = classifyExtension(path, k); if (!es.ok()) return es;
  std::vector<uint8_t> bytes;
  try {
    Status rs = readFileBounded(path, lim.maxFileBytes, bytes); if (!rs.ok()) return rs;
  } catch (const std::bad_alloc&) { return Status::Fail(Err::ResourceExhausted, "out of memory reading media"); }
  return validateAndDecodeGif(bytes.data(), bytes.size(), path, lim, out);
}

}} // namespace ss::media
