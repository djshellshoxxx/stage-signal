#include "ss/compose/overlay.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "ss/compose/font5x7.hpp"
#include "ss/compose/raster.hpp"

namespace ss::compose {

OutputPolicy policyFromRoute(const ss::OutputRoute& r) {
    return OutputPolicy{r.showText, r.showBpmKey, r.showBranding, r.showDiagnostics};
}

void OverlayInfo::setBrandingText(const char* s) {
    brandingText.fill(0);
    std::strncpy(brandingText.data(), s ? s : "", brandingText.size() - 1);
}
void OverlayInfo::addDiagnostic(const char* s) {
    if (diagnosticCount >= int(Limits::kMaxOverlayLines)) return;
    auto& l = diagnostics[std::size_t(diagnosticCount++)];
    l.fill(0);
    std::strncpy(l.data(), s ? s : "", l.size() - 1);
}

namespace {
const char* sourceName(ss::ValueSource s) {
    switch (s) {
        case ss::ValueSource::Manual: return "Manual";
        case ss::ValueSource::Estimated: return "Estimated";
        case ss::ValueSource::Tap: return "Tap";
        default: return "Unknown";
    }
}
const char* kKeyNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

bool containsNoCase(const char* hay, const char* needle) {
    const std::size_t n = std::strlen(needle);
    for (; *hay; ++hay) {
        std::size_t i = 0;
        while (i < n && hay[i] && (hay[i] | 0x20) == (needle[i] | 0x20)) ++i;
        if (i == n) return true;
    }
    return false;
}
bool isAlnum(char c) { return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'); }

// Draw one text line with a translucent dark backing; returns the box height.
Rect drawLabel(PixelBuffer& c, int x, int y, const char* text, int s, Rgb fg, Rgb bgc, std::uint32_t bgAlpha) {
    const int w = textWidth(text, s) + 2 * s, h = textHeight(s) + 2 * s;
    const Rect box{x, y, w, h};
    fillRect(c, box, premul(bgc, bgAlpha), true);
    drawText(c, x + s, y + s, text, s, fg, 255);
    return box;
}
}  // namespace

bool formatTempoLabel(const ss::TempoState& t, char* out, std::size_t cap) {
    if (!t.valid() || !(t.bpm > 0.0) || !(t.bpm < 1000.0)) return false;
    const long tenths = std::lround(t.bpm * 10.0);
    if (tenths % 10 == 0)
        std::snprintf(out, cap, "BPM %ld (%s)", tenths / 10, sourceName(t.source));
    else
        std::snprintf(out, cap, "BPM %ld.%ld (%s)", tenths / 10, tenths % 10, sourceName(t.source));
    return true;
}

bool formatKeyLabel(const ss::KeyState& k, char* out, std::size_t cap) {
    if (!k.valid() || k.tonic > 11) return false;
    std::snprintf(out, cap, "Key %s%s (%s)", kKeyNames[k.tonic], k.minor ? "m" : "", sourceName(k.source));
    return true;
}

bool looksSensitive(const char* line) {
    static const char* const kSecretWords[] = {"secret", "token", "passw", "streamkey", "stream_key", "stream key",
                                               "apikey", "api_key", "bearer", "rtmp://", "rtmps://", "srt://", "key="};
    for (const char* w : kSecretWords)
        if (containsNoCase(line, w)) return true;
    for (const char* p = line; *p; ++p) {
        if (*p == '\\') return true;
        if (*p == '/' && isAlnum(p[1]) && (p == line || p[-1] == ' ' || p[-1] == '=' || p[-1] == ':' || p[-1] == '"')) return true;
        if (*p == ':' && (p[1] == '/' || p[1] == '\\') && p > line && isAlnum(p[-1])) return true;
    }
    return false;
}

void drawOverlays(PixelBuffer& c, const OutputPolicy& pol, const OverlayInfo& info) {
    if (c.empty()) return;
    const int W = int(c.width), H = int(c.height);
    const int s = std::max(1, H / 150);
    const int m = 2 * s;
    char buf[64];

    if (pol.showBpmKey) {
        int y = m;
        if (formatTempoLabel(info.tempo, buf, sizeof buf)) y += drawLabel(c, m, y, buf, s, Rgb{255, 255, 255}, Rgb{0, 0, 0}, 160).h + s;
        if (formatKeyLabel(info.key, buf, sizeof buf)) drawLabel(c, m, y, buf, s, Rgb{255, 255, 255}, Rgb{0, 0, 0}, 160);
    }
    if (pol.showDiagnostics && info.clipWarning) {
        const char* t = "CLIP";
        const int w = textWidth(t, s) + 2 * s;
        drawLabel(c, W - m - w, m, t, s, Rgb{255, 255, 255}, Rgb{200, 0, 0}, 230);
    }
    if (pol.showBranding) {
        int right = W - m;
        const int bottom = H - m;
        if (info.brandingLogo && !info.brandingLogo->empty()) {
            const int maxH = std::max(1, H / 5);
            const PixelBuffer& lg = *info.brandingLogo;
            const int lh = std::min(maxH, int(lg.height));
            const int lw = std::max(1, int((std::int64_t(lg.width) * lh + lg.height / 2) / lg.height));
            DrawParams dp;
            drawScaled(lg, Rect{0, 0, int(lg.width), int(lg.height)}, c, Rect{right - lw, bottom - lh, lw, lh}, dp);
            right -= lw + s;
        }
        if (info.brandingText[0]) {
            const int w = textWidth(info.brandingText.data(), s) + 2 * s, h = textHeight(s) + 2 * s;
            drawLabel(c, right - w, bottom - h, info.brandingText.data(), s, Rgb{255, 255, 255}, Rgb{0, 0, 0}, 160);
        }
    }
    if (pol.showDiagnostics) {
        const int lineH = textHeight(s) + 2 * s + s;
        for (int i = 0; i < info.diagnosticCount; ++i) {
            const char* line = info.diagnostics[std::size_t(i)].data();
            const char* shown = looksSensitive(line) ? "[redacted]" : line;
            const int y = H - m - (i + 1) * lineH + s;
            drawLabel(c, m, y, shown, s, Rgb{255, 220, 0}, Rgb{0, 0, 0}, 190);
        }
    }
}

}  // namespace ss::compose
