#include "ss/diag/text.hpp"

#include <cmath>
#include "ss/base/utf8.hpp"

namespace ss::diag {

std::string htmlEscape(const std::string& s) {
    const bool valid = utf8::isValid(s);
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (c) {
            case '&': out += "&amp;"; continue;
            case '<': out += "&lt;"; continue;
            case '>': out += "&gt;"; continue;
            case '"': out += "&quot;"; continue;
            case '\'': out += "&#39;"; continue;
            case '`': out += "&#96;"; continue;
            default: break;
        }
        if (u < 0x20 && c != '\n' && c != '\t') continue;
        if (u == 0x7F) continue;
        if (u >= 0x80 && !valid) { out += '?'; continue; }
        out += c;
    }
    return out;
}

std::string csvText(const std::string& s) {
    std::string v = s;
    if (!v.empty() && (v[0] == '=' || v[0] == '+' || v[0] == '-' || v[0] == '@' || v[0] == '\t' || v[0] == '\r')) v.insert(v.begin(), '\'');
    const bool quote = v.find_first_of(",\"\r\n") != std::string::npos || (!v.empty() && (v[0] == ' ' || v.back() == ' '));
    if (!quote) return v;
    std::string out = "\"";
    for (char c : v) { if (c == '"') out += "\"\""; else out += c; }
    out += '"';
    return out;
}

std::string csvNumber(const std::string& f) {
    for (char c : f) if (!((c >= '0' && c <= '9') || c == '.' || c == '-')) return std::string();
    return f;
}

std::string fixedDecimals(double v, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    if (!std::isfinite(v)) return "0";
    std::int64_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    const bool neg = v < 0;
    const double a = std::fabs(v) * static_cast<double>(scale);
    if (a > 9e15) return neg ? "-9000000000000000" : "9000000000000000";
    const std::int64_t n = static_cast<std::int64_t>(a + 0.5);
    std::string out = std::to_string(n / scale);
    if (decimals > 0) {
        std::string frac = std::to_string(n % scale);
        out += "." + std::string(static_cast<std::size_t>(decimals) - frac.size(), '0') + frac;
    }
    if (neg && n != 0) out.insert(out.begin(), '-');
    return out;
}

std::string formatUtcIso(std::int64_t unixMs, bool withMillis) {
    std::int64_t secs = unixMs / 1000, ms = unixMs % 1000;
    if (ms < 0) { ms += 1000; --secs; }
    std::int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }
    // civil-from-days (Howard Hinnant)
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    auto two = [](std::int64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    std::string out = std::to_string(y) + "-" + two(m) + "-" + two(d) + "T" + two(rem / 3600) + ":" + two(rem % 3600 / 60) + ":" + two(rem % 60);
    if (withMillis) out += "." + std::string(ms < 10 ? "00" : ms < 100 ? "0" : "") + std::to_string(ms);
    return out + "Z";
}

std::string formatDuration(Ns ns) {
    std::int64_t s = ns < 0 ? 0 : ns / kNsPerSec;
    const std::int64_t h = s / 3600, m = s % 3600 / 60, sec = s % 60;
    auto two = [](std::int64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    return h > 0 ? std::to_string(h) + ":" + two(m) + ":" + two(sec) : std::to_string(m) + ":" + two(sec);
}

std::string keyName(int tonic, bool minor) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (tonic < 0 || tonic > 11) return std::string();
    return std::string(names[tonic]) + (minor ? " minor" : " major");
}

}  // namespace ss::diag
