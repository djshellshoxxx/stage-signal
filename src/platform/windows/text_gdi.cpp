// GDI text rasterizer: UTF-32 input, word wrap, grayscale antialiasing (ClearType off), alpha from a DIB section.
// Limitation: GDI TextOut does not do full complex-script shaping/bidi reordering (Arabic/Indic); documented, not claimed.
#include "win_internal.h"
#include <algorithm>
#include <cstring>

namespace ss { namespace win {
using namespace detail;
namespace {

std::wstring utf32ToUtf16(const std::u32string& s) {
  std::wstring w; w.reserve(s.size() + 8);
  for (char32_t c : s) {
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
    if (c < 0x10000) w.push_back((wchar_t)c);
    else { c -= 0x10000; w.push_back((wchar_t)(0xD800 + (c >> 10))); w.push_back((wchar_t)(0xDC00 + (c & 0x3FF))); }
  }
  return w;
}
bool isHigh(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }

class GdiText : public ITextRasterizer {
 public:
  bool rasterize(const std::u32string& text, const TextStyle& st, int maxW, int& outW, int& outH, std::vector<uint32_t>& argb) override {
    outW = outH = 0; argb.clear();
    if (maxW <= 0 || maxW > 8192 || st.px < 4 || st.px > 1024) return false;
    std::wstring w = utf32ToUtf16(text);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT font = createFont(dc, st);
    if (!font) { DeleteDC(dc); return false; }
    HGDIOBJ oldFont = SelectObject(dc, font);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    const int lineH = tm.tmHeight + tm.tmExternalLeading;
    const int pad = std::max(1, st.px / 16);              // room for overhang
    const int availW = std::max(1, maxW - 2 * pad);

    // ---- layout: lines (wstring) wrapped to availW
    std::vector<std::wstring> lines;
    size_t i = 0;
    while (i <= w.size()) {
      size_t e = w.find(L'\n', i); if (e == std::wstring::npos) e = w.size();
      std::wstring para = w.substr(i, e - i);
      if (!para.empty() && para.back() == L'\r') para.pop_back();
      wrap(dc, para, availW, lines);
      if (e >= w.size()) break;
      i = e + 1;
    }
    if (lines.size() > 512) lines.resize(512);
    int widest = 0;
    std::vector<int> widths(lines.size());
    for (size_t k = 0; k < lines.size(); ++k) { widths[k] = measure(dc, lines[k]); widest = std::max(widest, widths[k]); }
    int W = std::min(maxW, widest + 2 * pad), H = (int)lines.size() * lineH + 2 * pad;
    if (W < 1) W = 1;
    if ((long long)W * H > 16LL * 1024 * 1024 || H > 16384) { cleanup(dc, font, oldFont); return false; }

    BITMAPINFO bi; ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); cleanup(dc, font, oldFont); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    std::memset(bits, 0, (size_t)W * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    for (size_t k = 0; k < lines.size(); ++k)
      if (!lines[k].empty()) ExtTextOutW(dc, pad, pad + (int)k * lineH, 0, nullptr, lines[k].c_str(), (UINT)lines[k].size(), nullptr);
    GdiFlush();
    // coverage = green channel of white-on-black grayscale AA; emit non-premultiplied ARGB in requested colour
    const uint32_t* src = reinterpret_cast<const uint32_t*>(bits);
    const uint32_t rgbc = st.color & 0xFFFFFFu;
    argb.resize((size_t)W * H);
    for (size_t p = 0; p < (size_t)W * H; ++p) {
      uint32_t a = (src[p] >> 8) & 0xFF;
      argb[p] = a ? ((a << 24) | rgbc) : 0u;
    }
    outW = W; outH = H;
    SelectObject(dc, oldBmp); DeleteObject(bmp);
    cleanup(dc, font, oldFont);
    return true;
  }

 private:
  static void cleanup(HDC dc, HFONT f, HGDIOBJ old) { SelectObject(dc, old); DeleteObject(f); DeleteDC(dc); }

  static HFONT createFont(HDC dc, const TextStyle& st) {
    const char* fallbacks[] = {"Segoe UI", "Tahoma", "Arial", "Microsoft Sans Serif"};
    std::vector<std::wstring> names;
    if (!st.font.empty()) names.push_back(widen(st.font));
    for (auto f : fallbacks) names.push_back(widen(f));
    for (size_t i = 0; i < names.size(); ++i) {
      HFONT f = CreateFontW(-st.px, 0, 0, 0, st.bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY /* never CLEARTYPE */, DEFAULT_PITCH | FF_DONTCARE, names[i].c_str());
      if (!f) continue;
      HGDIOBJ old = SelectObject(dc, f);
      wchar_t face[LF_FACESIZE + 1] = {0};
      GetTextFaceW(dc, LF_FACESIZE, face);
      SelectObject(dc, old);
      if (_wcsicmp(face, names[i].c_str()) == 0 || i + 1 == names.size()) return f;   // exact face, or last resort (system default)
      DeleteObject(f);
    }
    return CreateFontW(-st.px, 0, 0, 0, st.bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"");
  }

  static int measure(HDC dc, const std::wstring& s) {
    if (s.empty()) return 0;
    SIZE sz; if (!GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz)) return 0;
    return sz.cx;
  }

  // Greedy wrap on spaces; words wider than the line are broken between code points (never inside a surrogate pair).
  static void wrap(HDC dc, const std::wstring& para, int maxW, std::vector<std::wstring>& out) {
    if (para.empty()) { out.push_back(std::wstring()); return; }
    std::wstring line; size_t i = 0;
    while (i < para.size()) {
      size_t j = i;
      while (j < para.size() && para[j] != L' ') ++j;
      size_t k = j; while (k < para.size() && para[k] == L' ') ++k;     // word + trailing spaces
      std::wstring word = para.substr(i, k - i);
      std::wstring trial = line + word;
      std::wstring trialTrim = trial; while (!trialTrim.empty() && trialTrim.back() == L' ') trialTrim.pop_back();
      if (measure(dc, trialTrim) <= maxW) { line = trial; }
      else {
        if (!line.empty()) { while (!line.empty() && line.back() == L' ') line.pop_back(); out.push_back(line); line.clear(); }
        std::wstring wt = word; while (!wt.empty() && wt.back() == L' ') wt.pop_back();
        if (measure(dc, wt) <= maxW) line = word;
        else {                                                   // hard-break an over-long word
          std::wstring cur;
          for (size_t c = 0; c < wt.size();) {
            size_t len = (isHigh(wt[c]) && c + 1 < wt.size()) ? 2 : 1;
            std::wstring next = cur + wt.substr(c, len);
            if (!cur.empty() && measure(dc, next) > maxW) { out.push_back(cur); cur.clear(); next = wt.substr(c, len); }
            cur = next; c += len;
          }
          line = cur;
        }
      }
      i = k;
    }
    while (!line.empty() && line.back() == L' ') line.pop_back();
    out.push_back(line);
  }
};

} // namespace
std::unique_ptr<ITextRasterizer> makeTextRasterizer() { return std::unique_ptr<ITextRasterizer>(new GdiText()); }
}} // namespace
