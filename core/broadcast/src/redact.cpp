#include "ss/broadcast/redact.hpp"
#include <cctype>

namespace ss::broadcast {

std::string redactUrl(const std::string& url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return url;
    const std::string scheme = url.substr(0, schemeEnd + 3);
    std::size_t i = schemeEnd + 3;
    std::size_t authEnd = url.find_first_of("/?#", i);
    if (authEnd == std::string::npos) authEnd = url.size();
    std::string authority = url.substr(i, authEnd - i);
    const auto at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);          // drop userinfo
    std::string out = scheme + authority;
    const std::string rest = url.substr(authEnd);
    if (rest.empty()) return out;
    const auto q = rest.find_first_of("?#");
    const std::string path = rest.substr(0, q);
    out += path.size() > 1 ? std::string("/<redacted>") : path;
    if (q != std::string::npos) out += "?<redacted>";
    return out;
}

std::string redactText(const std::string& text, const std::vector<std::string>& secrets) {
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto s = text.find("://", pos);
        if (s == std::string::npos) { out.append(text, pos, std::string::npos); break; }
        std::size_t b = s;                                                   // walk back over the scheme letters
        while (b > pos && (std::isalnum(static_cast<unsigned char>(text[b - 1])) || text[b - 1] == '+' || text[b - 1] == '.' || text[b - 1] == '-')) --b;
        std::size_t e = s + 3;                                               // forward to whitespace/quote/bracket
        while (e < text.size() && !std::isspace(static_cast<unsigned char>(text[e])) && text[e] != '"' && text[e] != '\'' && text[e] != '<' && text[e] != '>') ++e;
        // a trailing ':' ',' '.' ';' belongs to the sentence, not the URL
        std::size_t trim = e;
        while (trim > s + 3 && (text[trim - 1] == ':' || text[trim - 1] == ',' || text[trim - 1] == '.' || text[trim - 1] == ';' || text[trim - 1] == ')')) --trim;
        out.append(text, pos, b - pos);
        out += redactUrl(text.substr(b, trim - b));
        out.append(text, trim, e - trim);
        pos = e;
    }
    for (const auto& sec : secrets) {
        if (sec.size() < 4) continue;
        std::size_t p = 0;
        while ((p = out.find(sec, p)) != std::string::npos) { out.replace(p, sec.size(), "***"); p += 3; }
    }
    return out;
}

void wipe(std::vector<std::string>& values) {
    for (auto& v : values) {
        volatile char* p = v.empty() ? nullptr : &v[0];
        for (std::size_t i = 0; p && i < v.size(); ++i) p[i] = 0;
        v.clear();
    }
    values.clear();
}

}  // namespace ss::broadcast
