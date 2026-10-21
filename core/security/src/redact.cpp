#include "ss/security/redact.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "ss/base/utf8.hpp"

namespace ss::security {
namespace {

std::string lowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool isAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool isIdentChar(char c) { return isAlnum(c) || c == '_' || c == '-' || c == '.'; }
bool startsWithMarker(const std::string& s, std::size_t pos) {
    return s.compare(pos, std::strlen(kRedactedMarker), kRedactedMarker) == 0;
}

// ---- 1. URLs ------------------------------------------------------------------------------------
bool looksLikeKey(const std::string& seg) {
    if (seg.size() < 16) return false;
    bool digit = false, alpha = false;
    for (char c : seg) {
        if (std::isdigit(static_cast<unsigned char>(c))) digit = true;
        else if (std::isalpha(static_cast<unsigned char>(c))) alpha = true;
        else if (c != '_' && c != '-') return false;
    }
    return digit && alpha;
}

bool isStreamScheme(const std::string& s) {
    return s == "rtmp" || s == "rtmps" || s == "rtsp" || s == "rtsps" || s == "srt" || s == "rtp";
}

std::string redactOneUrl(const std::string& url, int& count) {
    const std::size_t p = url.find("://");
    if (p == std::string::npos) return url;
    const std::string scheme = lowerAscii(url.substr(0, p));
    const std::string rest = url.substr(p + 3);
    const std::size_t aEnd = rest.find_first_of("/?#");
    std::string authority = rest.substr(0, aEnd);
    std::string tail = aEnd == std::string::npos ? std::string() : rest.substr(aEnd);
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) { authority = authority.substr(at + 1); ++count; }
    const std::size_t qEnd = tail.find_first_of("?#");
    std::string path = tail.substr(0, qEnd);
    if (qEnd != std::string::npos && qEnd + 1 < tail.size()) ++count;   // dropped query/fragment content
    std::string outPath;
    if (!path.empty()) {
        std::size_t i = 1, idx = 0;   // path starts with '/'
        outPath = "/";
        while (i <= path.size()) {
            std::size_t j = path.find('/', i);
            if (j == std::string::npos) j = path.size();
            const std::string seg = path.substr(i, j - i);
            const bool redactSeg = !seg.empty() && ((isStreamScheme(scheme) && idx >= 1) || looksLikeKey(seg));
            if (redactSeg) { outPath += kRedactedMarker; ++count; } else outPath += seg;
            if (j < path.size()) outPath += '/';
            i = j + 1; ++idx;
        }
    }
    return url.substr(0, p) + "://" + authority + outPath;
}

bool isUrlEnd(char c) {
    return static_cast<unsigned char>(c) <= ' ' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '`' || c == '\\';
}
bool isSchemeChar(char c) { return isAlnum(c) || c == '+' || c == '-' || c == '.'; }

std::string redactUrlsPass(const std::string& in, int& count) {
    std::string out;
    std::size_t i = 0;
    while (i < in.size()) {
        const std::size_t p = in.find("://", i);
        if (p == std::string::npos) { out.append(in, i, std::string::npos); break; }
        std::size_t s = p;
        while (s > i && isSchemeChar(in[s - 1])) --s;
        while (s < p && !std::isalpha(static_cast<unsigned char>(in[s]))) ++s;
        if (s == p) { out.append(in, i, p + 3 - i); i = p + 3; continue; }
        std::size_t e = p + 3;
        while (e < in.size() && !isUrlEnd(in[e])) ++e;
        while (e > p + 3 && std::strchr(".,;:!?)]}", in[e - 1]) != nullptr) --e;
        out.append(in, i, s - i);
        out += redactOneUrl(in.substr(s, e - s), count);
        i = e;
    }
    return out;
}

// ---- 2. filesystem usernames --------------------------------------------------------------------
std::size_t userPrefixLen(const std::string& s, std::size_t i) {
    auto m = [&](const char* lit) -> std::size_t {
        const std::size_t n = std::strlen(lit);
        return s.compare(i, n, lit) == 0 ? n : 0;
    };
    if (i > 0 && (isAlnum(s[i - 1]) || s[i - 1] == '_' || s[i - 1] == '-' || s[i - 1] == '.')) return 0;
    if (std::size_t n = m("/Users/")) return n;
    if (std::size_t n = m("/home/")) return n;
    if (std::size_t n = m("\\Users\\")) return n;
    if (i > 0 && s[i - 1] == ':') {   // C:\users\ or C:/users/ in any case
        const std::string low = lowerAscii(s.substr(i, 7));
        if (low == "\\users\\" || low == "/users/") return 7;
    }
    return 0;
}

std::string redactUserPaths(const std::string& in, int& count) {
    std::string out;
    std::size_t i = 0;
    while (i < in.size()) {
        const char c = in[i];
        std::size_t pre = (c == '/' || c == '\\') ? userPrefixLen(in, i) : 0;
        if (pre == 0) { out += c; ++i; continue; }
        std::size_t j = i + pre;
        while (j < in.size() && (in[j] == '\\' || in[j] == '/')) ++j;
        if (j >= in.size() || in[j] == '<') { out.append(in, i, pre); i += pre; continue; }
        std::size_t k = j;
        while (k < in.size() && std::strchr("/\\ \t\r\n\"'<>:,;)", in[k]) == nullptr) ++k;
        if (k == j) { out.append(in, i, pre); i += pre; continue; }
        out.append(in, i, j - i);
        out += kUserMarker;
        ++count;
        i = k;
    }
    return out;
}

// ---- 3. secret-like option values ---------------------------------------------------------------
std::string normName(const std::string& ident) {
    std::string n;
    for (char c : ident) {
        if (c == '_' || c == '-' || c == '.') continue;
        n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return n;
}
bool endsWith(const std::string& s, const char* suf) {
    const std::size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
bool isWeakName(const std::string& n) { return n == "key" || n == "pass" || n == "auth" || n == "sig"; }
bool isSecretName(const std::string& n) {
    static const char* exact[] = {"key", "apikey", "streamkey", "privatekey", "accesskey", "secretkey", "sharedkey",
        "authkey", "token", "accesstoken", "refreshtoken", "authtoken", "bearertoken", "idtoken", "secret",
        "clientsecret", "password", "passwd", "pwd", "pass", "passphrase", "passcode", "auth", "authorization",
        "credential", "credentials", "streamid", "sig", "signature", "cookie", "setcookie", "sessionid"};
    for (const char* e : exact) if (n == e) return true;
    return endsWith(n, "password") || endsWith(n, "passphrase") || endsWith(n, "secret") || endsWith(n, "token") ||
           endsWith(n, "apikey") || endsWith(n, "streamkey") || endsWith(n, "privatekey") || endsWith(n, "accesskey") ||
           endsWith(n, "secretkey");
}

bool isValueStop(char c) { return isSpace(c) || std::strchr("&,;\"')}]", c) != nullptr; }

std::string redactOptionValues(const std::string& in, int& count) {
    std::string out;
    const std::size_t n = in.size();
    std::size_t i = 0;
    while (i < n) {
        if (!(isIdentChar(in[i]) && (i == 0 || !isIdentChar(in[i - 1])))) { out += in[i]; ++i; continue; }
        std::size_t e = i;
        while (e < n && isIdentChar(in[e])) ++e;
        std::size_t nameStart = i;
        bool dashed = false;
        while (nameStart < e && in[nameStart] == '-') { dashed = true; ++nameStart; }
        std::size_t nameEnd = e;
        while (nameEnd > nameStart && (in[nameEnd - 1] == '.' || in[nameEnd - 1] == '-')) --nameEnd;
        const std::string ident = in.substr(nameStart, nameEnd - nameStart);
        const std::string norm = normName(ident);

        // standalone "Bearer <token>"
        if (norm == "bearer" && e < n && in[e] == ' ') {
            std::size_t k = e + 1;
            while (k < n && (isAlnum(in[k]) || std::strchr("._~+/=-", in[k]) != nullptr)) ++k;
            if (k - (e + 1) >= 8) {
                out.append(in, i, e + 1 - i);
                out += kRedactedMarker;
                ++count;
                i = k;
                continue;
            }
        }
        if (!isSecretName(norm)) { out.append(in, i, e - i); i = e; continue; }

        std::size_t j = e;
        bool quotedName = false;
        if (j < n && (in[j] == '"' || in[j] == '\'')) { quotedName = true; ++j; }
        std::size_t afterName = j;
        while (j < n && (in[j] == ' ' || in[j] == '\t')) ++j;
        bool haveSep = false;
        if (j < n && (in[j] == '=' || in[j] == ':')) {
            const bool colon = in[j] == ':';
            if (colon && j + 2 < n && in[j + 1] == '/' && in[j + 2] == '/') { out.append(in, i, e - i); i = e; continue; }
            if (colon && isWeakName(norm) && !quotedName) { out.append(in, i, e - i); i = e; continue; }
            haveSep = true;
            ++j;
        } else if (dashed && j > afterName) {
            haveSep = true;   // "-passphrase value"
        }
        if (!haveSep) { out.append(in, i, e - i); i = e; continue; }
        while (j < n && (in[j] == ' ' || in[j] == '\t')) ++j;
        if (j >= n || in[j] == '\n' || in[j] == '\r') { out.append(in, i, e - i); i = e; continue; }
        if (dashed && in[j] == '-') { out.append(in, i, e - i); i = e; continue; }

        std::size_t vs = j, ve;
        char quote = 0;
        if (in[j] == '"' || in[j] == '\'') {
            quote = in[j];
            vs = j + 1;
            ve = vs;
            while (ve < n && in[ve] != quote && in[ve] != '\n') { if (in[ve] == '\\' && ve + 1 < n) ++ve; ++ve; }
            if (ve > n) ve = n;
        } else {
            ve = vs;
            while (ve < n && !isValueStop(in[ve])) ++ve;
            const std::string v = lowerAscii(in.substr(vs, ve - vs));
            if ((v == "bearer" || v == "basic" || v == "digest" || v == "token") && ve < n && in[ve] == ' ') {
                std::size_t k = ve + 1;
                while (k < n && !isValueStop(in[k])) ++k;
                if (k > ve + 1) ve = k;
            }
        }
        if (ve == vs || startsWithMarker(in, vs)) { out.append(in, i, ve > i ? ve - i : e - i); i = ve > i ? ve : e; continue; }
        out.append(in, i, vs - i);
        out += kRedactedMarker;
        ++count;
        i = ve;
        (void)quote;
    }
    return out;
}

}  // namespace

RedactionResult redact(const std::string& in) {
    RedactionResult r;
    std::string s = redactUrlsPass(in, r.replacements);
    s = redactUserPaths(s, r.replacements);
    s = redactOptionValues(s, r.replacements);
    r.text = std::move(s);
    return r;
}

std::string redactUrl(const std::string& url) { return redact(url).text; }

std::string truncateUtf8(const std::string& in, std::size_t maxBytes) {
    if (in.size() <= maxBytes) return in;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(in[cut]) & 0xC0) == 0x80) --cut;
    return in.substr(0, cut);
}

std::string sanitizeForLog(const std::string& in, std::size_t maxBytes) {
    std::string s = redact(in).text;
    const bool valid = utf8::isValid(s);
    for (auto& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) c = ' ';
        else if (u >= 0x80 && !valid) c = '?';
    }
    return truncateUtf8(s, maxBytes);
}

void Redactor::addLiteral(const std::string& secret) {
    if (secret.size() < 4) return;
    if (std::find(literals_.begin(), literals_.end(), secret) != literals_.end()) return;
    literals_.push_back(secret);
    std::sort(literals_.begin(), literals_.end(),
              [](const std::string& a, const std::string& b) { return a.size() != b.size() ? a.size() > b.size() : a < b; });
}

RedactionResult Redactor::apply(const std::string& in) const {
    std::string s = in;
    int count = 0;
    for (const auto& lit : literals_) {
        std::size_t pos = 0;
        while ((pos = s.find(lit, pos)) != std::string::npos) {
            s.replace(pos, lit.size(), kRedactedMarker);
            pos += std::strlen(kRedactedMarker);
            ++count;
        }
    }
    RedactionResult r = redact(s);
    r.replacements += count;
    return r;
}

}  // namespace ss::security
