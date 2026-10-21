// Test fixtures: tiny synthetic file headers. No real decoders and no files on disk needed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fx {

inline void be32(std::string& s, std::uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back(char((v >> (8 * i)) & 0xFF)); }
inline void le32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xFF)); }
inline void le16(std::string& s, std::uint32_t v) { s.push_back(char(v & 0xFF)); s.push_back(char((v >> 8) & 0xFF)); }

inline std::string png(std::uint32_t w, std::uint32_t h) {
    std::string s("\x89PNG\r\n\x1a\n", 8);
    be32(s, 13); s += "IHDR"; be32(s, w); be32(s, h);
    s += std::string("\x08\x06\x00\x00\x00", 5); be32(s, 0);
    return s;
}
inline std::string jpeg(std::uint32_t w, std::uint32_t h) {
    std::string s("\xFF\xD8", 2);
    s += std::string("\xFF\xE0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00", 18);
    s += std::string("\xFF\xC0\x00\x11\x08", 5);
    s.push_back(char(h >> 8)); s.push_back(char(h & 0xFF)); s.push_back(char(w >> 8)); s.push_back(char(w & 0xFF));
    s += std::string("\x03\x01\x11\x00\x02\x11\x01\x03\x11\x01", 10);
    return s;
}
inline std::string bmp(std::int32_t w, std::int32_t h) {
    std::string s = "BM";
    le32(s, 70); le32(s, 0); le32(s, 54); le32(s, 40);
    le32(s, std::uint32_t(w)); le32(s, std::uint32_t(h));
    le16(s, 1); le16(s, 24); le32(s, 0); le32(s, 0); le32(s, 0); le32(s, 0); le32(s, 0); le32(s, 0);
    return s;
}
// delaysCs: centiseconds per frame (0 = frame without a graphic control block when noGce). loop: -1 none.
inline std::string gif(std::uint32_t w, std::uint32_t h, const std::vector<int>& delaysCs, int loop = -1) {
    std::string s = "GIF89a";
    le16(s, w); le16(s, h); s.push_back(char(0x00)); s.push_back(0); s.push_back(0);
    if (loop >= 0) {
        s += std::string("\x21\xFF\x0B", 3); s += "NETSCAPE2.0"; s += std::string("\x03\x01", 2); le16(s, std::uint32_t(loop)); s.push_back(0);
    }
    for (int d : delaysCs) {
        s += std::string("\x21\xF9\x04\x00", 4); le16(s, std::uint32_t(d)); s += std::string("\x00\x00", 2);
        s.push_back(char(0x2C)); le16(s, 0); le16(s, 0); le16(s, w); le16(s, h); s.push_back(0);
        s.push_back(2); s.push_back(2); s.push_back(char(0x44)); s.push_back(char(0x01)); s.push_back(0);   // LZW size + 2-byte sub-block
    }
    s.push_back(char(0x3B));
    return s;
}
inline std::string mp4(const char* brand = "isom") {
    std::string s("\x00\x00\x00\x18" "ftyp", 8);
    s += brand; s += std::string("\x00\x00\x02\x00", 4); s += "isomiso2";
    return s;
}
inline std::string mkv(const char* doctype = "matroska") {
    std::string s("\x1A\x45\xDF\xA3\x9F\x42\x86\x81\x01\x42\xF7\x81\x01\x42\xF2\x81\x04\x42\xF3\x81\x08\x42\x82", 23);
    s.push_back(char(0x80 | std::string(doctype).size())); s += doctype;
    s.append(64, '\0');
    return s;
}

}  // namespace fx
