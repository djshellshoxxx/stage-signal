// StageSignal base: minimal strict UTF-8 helpers (text limits: <= 4096 code points per message).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ss::utf8 {

// Returns number of code points, or -1 when the input is not well-formed UTF-8 (overlongs, surrogates,
// > U+10FFFF and truncated sequences are rejected).
inline long countCodePoints(const std::string& s) {
    long n = 0; std::size_t i = 0; const std::size_t len = s.size();
    while (i < len) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t extra; std::uint32_t cp, minv;
        if (c < 0x80) { extra = 0; cp = c; minv = 0; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; minv = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; minv = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; minv = 0x10000; }
        else return -1;
        for (std::size_t k = 1; k <= extra; ++k) {
            if (i + k >= len) return -1;
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < minv || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
        i += extra + 1; ++n;
    }
    return n;
}
inline bool isValid(const std::string& s) { return countCodePoints(s) >= 0; }

}  // namespace ss::utf8
