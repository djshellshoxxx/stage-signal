#include "ss/record/naming.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace ss::record {
namespace {
bool reservedName(const std::string& s) {
    std::string up;
    for (char c : s) { if (c == '.') break; up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c)))); }
    if (up == "CON" || up == "PRN" || up == "AUX" || up == "NUL") return true;
    if (up.size() == 4 && (up.compare(0, 3, "COM") == 0 || up.compare(0, 3, "LPT") == 0) && up[3] >= '0' && up[3] <= '9') return true;
    return false;
}
// Howard Hinnant's civil_from_days.
void civilFromDays(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2));
}
std::string stem(Ns wall, const std::string& id, const std::string& show) {
    return formatUtcCompact(wall) + "_" + id + "_" + sanitizeShowName(show);
}
}  // namespace

std::string sanitizeShowName(const std::string& in, std::size_t maxBytes) {
    std::string out;
    for (unsigned char c : in) {
        const bool ok = std::isalnum(c) || c == '.' || c == '-' || c == '_';
        const char r = ok && c < 0x80 ? static_cast<char>(c) : '_';
        if (r == '_' && !out.empty() && out.back() == '_') continue;
        out.push_back(r);
    }
    auto trim = [](char c) { return c == '_' || c == '.' || c == ' '; };
    while (!out.empty() && trim(out.back())) out.pop_back();
    std::size_t b = 0;
    while (b < out.size() && trim(out[b])) ++b;
    out.erase(0, b);
    if (out.size() > maxBytes) out.resize(maxBytes);
    while (!out.empty() && trim(out.back())) out.pop_back();
    if (out.empty()) out = "show";
    if (reservedName(out)) out.insert(out.begin(), '_');
    return out;
}

std::string formatUtcCompact(Ns wallUnixNs) {
    std::int64_t secs = wallUnixNs / kNsPerSec;
    if (wallUnixNs < 0 && wallUnixNs % kNsPerSec != 0) --secs;
    std::int64_t days = secs / 86400;
    std::int64_t rem = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }
    int y; unsigned m, d;
    civilFromDays(days, y, m, d);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02u%02uT%02d%02d%02dZ", y, m, d, int(rem / 3600), int((rem % 3600) / 60), int(rem % 60));
    return buf;
}

std::string makeSessionId(Rng& rng) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    const std::uint64_t v = rng.next();
    for (int i = 0; i < 8; ++i) s.push_back(hex[(v >> (4 * i)) & 0xF]);
    return s;
}
bool isValidSessionId(const std::string& id) {
    return id.size() == 8 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string segmentFileName(Ns wall, const std::string& id, const std::string& show, std::uint32_t idx, const std::string& ext) {
    char n[16];
    std::snprintf(n, sizeof n, "%04u", idx);
    return stem(wall, id, show) + "_seg" + n + "." + ext;
}
std::string segmentPattern(Ns wall, const std::string& id, const std::string& show, const std::string& ext) {
    std::string s = stem(wall, id, show), esc;
    for (char c : s) { esc.push_back(c); if (c == '%') esc.push_back('%'); }
    return esc + "_seg%04d." + ext;
}
std::string journalFileName(Ns wall, const std::string& id, const std::string& show) { return stem(wall, id, show) + ".journal"; }
std::string segmentListFileName(Ns wall, const std::string& id, const std::string& show) { return stem(wall, id, show) + ".segments.csv"; }
std::string metadataFileName(Ns wall, const std::string& id, const std::string& show) { return stem(wall, id, show) + ".session.json"; }

}  // namespace ss::record
