#include "core/security.h"
#include <cctype>
#include <cstring>
#include <algorithm>
#include <filesystem>

namespace ss { namespace sec {
namespace {

inline bool isAlpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }
inline bool isAlnum(unsigned char c) { return isAlpha(c) || isDigit(c); }
inline bool isHex(unsigned char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
inline bool isSpace(unsigned char c) { return c <= 0x20 || c == 0x7f; }
inline bool isSep(char c) { return c == '/' || c == '\\'; }
std::string lower(const std::string& s) { std::string o = s; for (auto& c : o) c = lc(c); return o; }

// ---------------- URLs ----------------
inline bool isSchemeChar(unsigned char c) { return isAlnum(c) || c == '+' || c == '-' || c == '.'; }
inline bool isUrlStop(unsigned char c) { return isSpace(c) || c == '"' || c == '\'' || c == '<' || c == '>' || c == '`'; }

bool isStreamScheme(const std::string& s) {
  static const char* k[] = {"rtmp", "rtmps", "rtmpe", "rtmpt", "rtmpte", "rtmpts", "rtsp", "rtsps", "srt", "rtp", "srtp",
                            "udp", "rist", "mmsh", "mmst", "mms", nullptr};
  for (int i = 0; k[i]; ++i) if (s == k[i]) return true;
  return false;
}
bool isKeyIntroducer(const std::string& seg) {
  static const char* k[] = {"key", "keys", "token", "tokens", "secret", "stream", "streamkey", "apikey", "api_key", "auth",
                            "password", "pass", "access", "sig", "signature", "s", "k", "t", "live", nullptr};
  std::string l = lower(seg);
  for (int i = 0; k[i]; ++i) if (l == k[i]) return true;
  return false;
}
bool keyLikeSegment(const std::string& seg) {
  if (seg.size() < 16) return false;
  bool dig = false, alp = false;
  for (unsigned char c : seg) {
    if (isDigit(c)) dig = true; else if (isAlpha(c)) alp = true;
    else if (c != '_' && c != '-' && c != '.' && c != '~' && c != '%' && c != '=') return false;
  }
  return seg.size() >= 32 || (dig && alp);
}

std::string redactOneUrlRest(const std::string& scheme, const std::string& rest) {
  // userinfo: up to the LAST '@' that occurs before the first '?' or '#'.
  size_t qpos = rest.find_first_of("?#");
  size_t limit = qpos == std::string::npos ? rest.size() : qpos;
  size_t at = rest.rfind('@', limit == 0 ? 0 : limit - 1);
  bool hasUser = (at != std::string::npos && at < limit);
  size_t start = hasUser ? at + 1 : 0;
  size_t authEnd = rest.find_first_of("/\\?#", start);
  if (authEnd == std::string::npos) authEnd = rest.size();
  std::string out = hasUser ? std::string(kRedacted) + "@" : std::string();
  out += rest.substr(start, authEnd - start);
  // path
  size_t pathEnd = rest.find_first_of("?#", authEnd);
  if (pathEnd == std::string::npos) pathEnd = rest.size();
  std::string path = rest.substr(authEnd, pathEnd - authEnd);
  if (!path.empty()) {
    bool stream = isStreamScheme(scheme);
    std::string np; size_t i = 0; int segIdx = 0; std::string prev;
    while (i <= path.size()) {
      size_t j = i; while (j < path.size() && !isSep(path[j])) ++j;
      std::string seg = path.substr(i, j - i);
      if (!seg.empty()) {
        bool red = stream ? segIdx >= 1 : (isKeyIntroducer(prev) || keyLikeSegment(seg));
        if (seg == kRedacted) red = true;
        np += red ? std::string(kRedacted) : seg;
        prev = seg; ++segIdx;
      }
      if (j < path.size()) np += path[j];
      i = j + 1;
    }
    out += np;
  }
  if (pathEnd < rest.size()) {
    size_t hash = rest.find('#', pathEnd);
    if (rest[pathEnd] == '?') out += "?" + std::string(kRedacted);
    if (hash != std::string::npos) out += "#" + std::string(kRedacted);
  }
  return out;
}

// ---------------- secret options ----------------
inline bool isIdChar(unsigned char c) { return isAlnum(c) || c == '_' || c == '-' || c == '.'; }

bool isSecretName(const std::string& id) {
  // id: identifier run without leading dashes.
  static const char* longKw[] = {"password", "passwd", "passphrase", "secret", "token", "apikey", "streamkey", "streamid",
                                 "credential", "credentials", "privatekey", "authorization", "bearer", "cookie", "signature",
                                 "sessionid", "accesskey", "secretkey", "psk", nullptr};
  static const char* shortKw[] = {"key", "sig", "pwd", "pass", "auth", "pw", nullptr};
  std::string compact;
  for (char c : id) if (c != '_' && c != '-' && c != '.') compact += lc(c);
  for (int i = 0; longKw[i]; ++i) {
    size_t n = std::strlen(longKw[i]);
    if (compact.size() >= n && compact.compare(compact.size() - n, n, longKw[i]) == 0) return true;
  }
  // last sub-token (split on _-. and camelCase)
  size_t e = id.size(), b = e;
  while (b > 0 && id[b - 1] != '_' && id[b - 1] != '-' && id[b - 1] != '.') {
    --b;
    if (b > 0 && (unsigned char)id[b] >= 'A' && (unsigned char)id[b] <= 'Z' && (unsigned char)id[b - 1] >= 'a' && (unsigned char)id[b - 1] <= 'z') break;
  }
  std::string last = lower(id.substr(b, e - b));
  for (int i = 0; shortKw[i]; ++i) if (last == shortKw[i]) return true;
  return false;
}

bool isAuthWord(const std::string& v) {
  std::string l = lower(v);
  return l == "bearer" || l == "basic" || l == "token" || l == "digest" || l == "negotiate";
}

} // namespace

std::string redactUrls(const std::string& s) {
  std::string out; out.reserve(s.size());
  size_t last = 0, i = 0;
  while ((i = s.find("://", i)) != std::string::npos) {
    size_t ss = i; while (ss > last && isSchemeChar((unsigned char)s[ss - 1])) --ss;
    while (ss < i && !isAlpha((unsigned char)s[ss])) ++ss;
    size_t restBeg = i + 3, e = restBeg;
    while (e < s.size() && !isUrlStop((unsigned char)s[e])) ++e;
    std::string scheme = lower(s.substr(ss, i - ss));
    out.append(s, last, ss - last);
    out += s.substr(ss, i - ss) + "://" + redactOneUrlRest(scheme, s.substr(restBeg, e - restBeg));
    last = e; i = e;
  }
  out.append(s, last, std::string::npos);
  return out;
}

std::string redactSecretOptions(const std::string& s) {
  std::string out; out.reserve(s.size());
  size_t n = s.size(), i = 0, copied = 0;
  while (i < n) {
    if (!isIdChar((unsigned char)s[i])) { ++i; continue; }
    size_t a = i; while (i < n && isIdChar((unsigned char)s[i])) ++i;
    size_t b = i;
    size_t na = a; while (na < b && s[na] == '-') ++na;
    bool flag = na > a;
    std::string id = s.substr(na, b - na);
    if (id.empty() || !isSecretName(id)) continue;
    size_t j = b;
    if (j < n && (s[j] == '"' || s[j] == '\'')) ++j;               // closing quote of a JSON/quoted key
    size_t k = j; while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
    size_t vstart; bool sep = false;
    if (k < n && (s[k] == '=' || s[k] == ':')) {
      sep = true; ++k;
      if (k < n && s[k] == '>' ) ++k;                               // =>
      while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
      vstart = k;
    } else if (flag && k > j && k < n && s[k] != '\n' && s[k] != '\r') {
      vstart = k;                                                   // --password hunter2
      if (s[k] == '-' && k + 1 < n && isAlpha((unsigned char)s[k + 1])) continue;   // next flag
    } else continue;
    if (vstart >= n || s[vstart] == '\n' || s[vstart] == '\r') continue;
    size_t vend;
    if (s[vstart] == '"' || s[vstart] == '\'') {
      char q = s[vstart]; size_t p = vstart + 1;
      while (p < n && s[p] != q && s[p] != '\n') { if (s[p] == '\\' && p + 1 < n) ++p; ++p; }
      out.append(s, copied, vstart + 1 - copied); out += kRedacted;
      if (p < n && s[p] == q) { out += q; ++p; }
      copied = p; i = p; continue;
    }
    vend = vstart; while (vend < n && !isSpace((unsigned char)s[vend]) && s[vend] != '"') ++vend;
    if (vend == vstart) continue;
    if (isAuthWord(s.substr(vstart, vend - vstart))) {
      size_t p = vend; while (p < n && (s[p] == ' ' || s[p] == '\t')) ++p;
      size_t q = p; while (q < n && !isSpace((unsigned char)s[q]) && s[q] != '"') ++q;
      if (q > p) vend = q;
    }
    (void)sep;
    out.append(s, copied, vstart - copied); out += kRedacted;
    copied = vend; i = vend;
  }
  out.append(s, copied, std::string::npos);
  return out;
}

std::string redactUserNames(const std::string& s) {
  std::string out; out.reserve(s.size());
  const std::string L = lower(s);
  size_t n = s.size(), i = 0, copied = 0;
  while (i < n) {
    if (!isSep(s[i])) { ++i; continue; }
    // preceding-context rule: previous char must not be alnum, except a drive letter after "/mnt/".
    bool ctxOk = true;
    if (i > 0 && isAlnum((unsigned char)s[i - 1])) {
      ctxOk = (i >= 6 && L.compare(i - 6, 5, "/mnt/") == 0 && isAlpha((unsigned char)s[i - 1]));
    }
    size_t runStart = i; size_t j = i; bool allFwd = true;
    while (j < n && isSep(s[j])) { if (s[j] != '/') allFwd = false; ++j; }
    if (!ctxOk) { i = j; continue; }
    (void)runStart;
    size_t nameLen = 0; bool isHome = false;
    if (L.compare(j, 5, "users") == 0) nameLen = 5;
    else if (L.compare(j, 4, "home") == 0) { nameLen = 4; isHome = true; }
    else if (L.compare(j, 22, "documents and settings") == 0) nameLen = 22;
    if (!nameLen || j + nameLen >= n || !isSep(s[j + nameLen])) { i = j; continue; }
    if (isHome && !allFwd) { i = j; continue; }
    size_t k = j + nameLen; while (k < n && isSep(s[k])) ++k;
    size_t u = k, m = u; int spaces = 0;
    auto term = [&](unsigned char c) { return isSep((char)c) || c == '"' || c == '\'' || c == '<' || c == '>' || c == '|' || c == '*' || c == '?' || c == ':' || c < 0x20 || c == 0x7f; };
    while (m < n) {
      unsigned char c = (unsigned char)s[m];
      if (term(c)) break;
      if (c == ' ') {
        // include spaces only when a separator follows soon ("John Smith\")
        size_t t = m + 1; bool found = false;
        while (t < n && t - m < 40 && !term((unsigned char)s[t])) ++t;
        if (t < n && isSep(s[t]) && spaces < 2) found = true;
        if (!found) break;
        ++spaces;
      }
      ++m;
    }
    if (m == u) { i = k; continue; }
    out.append(s, copied, u - copied); out += "<user>";
    copied = m; i = m;
  }
  out.append(s, copied, std::string::npos);
  return out;
}

std::string redactKeyShapes(const std::string& s) {
  std::string out; out.reserve(s.size());
  size_t n = s.size(), i = 0, copied = 0;
  while (i < n) {
    bool boundary = (i == 0) || !isIdChar((unsigned char)s[i - 1]);
    if (boundary && (s[i] == 'l' || s[i] == 'L') && i + 5 <= n && lower(s.substr(i, 5)) == "live_") {
      size_t p = i + 5; size_t d = p; while (d < n && isDigit((unsigned char)s[d])) ++d;
      if (d > p && d < n && s[d] == '_') {
        size_t q = d + 1; size_t e = q; while (e < n && isAlnum((unsigned char)s[e])) ++e;
        if (e - q >= 16) { out.append(s, copied, i - copied); out += kRedacted; copied = e; i = e; continue; }
      }
    }
    if (boundary && isAlnum((unsigned char)s[i])) {
      // 5 groups of 4 alnum separated by '-' (YouTube-style key), not part of a longer token
      size_t p = i; int g = 0; bool ok = true;
      for (; g < 5 && ok; ++g) {
        for (int c = 0; c < 4; ++c) { if (p < n && isAlnum((unsigned char)s[p])) ++p; else { ok = false; break; } }
        if (ok && g < 4) { if (p < n && s[p] == '-') ++p; else ok = false; }
      }
      if (ok && (p >= n || !isIdChar((unsigned char)s[p]))) {
        bool dg = false, al = false; for (size_t t = i; t < p; ++t) { if (isDigit((unsigned char)s[t])) dg = true; else if (isAlpha((unsigned char)s[t])) al = true; }
        if (dg && al) { out.append(s, copied, i - copied); out += kRedacted; copied = p; i = p; continue; }
      }
    }
    ++i;
  }
  out.append(s, copied, std::string::npos);
  return out;
}

bool isSecretKeyName(const std::string& name) {
  size_t b = 0; while (b < name.size() && name[b] == '-') ++b;
  return b < name.size() && isSecretName(name.substr(b));
}
std::string redact(const std::string& s) { return redactKeyShapes(redactUserNames(redactSecretOptions(redactUrls(s)))); }
bool looksSecret(const std::string& s) { return redact(s) != s; }

// ---- Redactor ----
void Redactor::addLiteral(const std::string& secret, const std::string& replacement) {
  if (secret.size() < 3) return;
  auto push = [&](const std::string& v) { if (v.size() >= 3) { std::string m = lower(v); for (auto& l : lits_) if (l.match == m) return; lits_.push_back({m, replacement}); } };
  push(secret);
  std::string pct, esc;
  for (unsigned char c : secret) {
    if (isAlnum(c) || c == '-' || c == '_' || c == '.' || c == '~') pct += (char)c; else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); pct += b; }
    if (c == '\\' || c == '"') esc += '\\';
    esc += (char)c;
  }
  push(pct); push(esc);
}
std::string Redactor::apply(const std::string& s) const {
  std::string cur = s;
  for (const auto& l : lits_) {
    std::string low = lower(cur), o; size_t pos = 0, f;
    while ((f = low.find(l.match, pos)) != std::string::npos) { o.append(cur, pos, f - pos); o += l.repl; pos = f + l.match.size(); }
    if (pos == 0) continue;
    o.append(cur, pos, std::string::npos); cur.swap(o);
  }
  return redact(cur);
}

std::string sanitizeLibraryError(const std::string& raw, size_t maxLen) {
  const size_t cap = 64 * 1024;
  std::string in = raw.size() > cap ? raw.substr(0, cap) : raw;
  std::string t; t.reserve(in.size());
  bool lastSpace = true;
  for (size_t i = 0; i < in.size();) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x80) {
      if (c <= 0x20 || c == 0x7f) { if (!lastSpace) t += ' '; lastSpace = true; } else { t += (char)c; lastSpace = false; }
      ++i; continue;
    }
    int len = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
    bool ok = len && i + len <= in.size();
    for (int k = 1; ok && k < len; ++k) ok = ((unsigned char)in[i + k] & 0xC0) == 0x80;
    if (ok && len == 3) { unsigned char c1 = (unsigned char)in[i + 1]; if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) ok = false; }
    if (ok && len == 4) { unsigned char c1 = (unsigned char)in[i + 1]; if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) ok = false; }
    if (ok) { t.append(in, i, len); i += len; lastSpace = false; } else { t += '?'; ++i; lastSpace = false; }
  }
  while (!t.empty() && t.back() == ' ') t.pop_back();
  std::string r = redact(t);
  if (r.size() > maxLen) {
    size_t cut = maxLen > 3 ? maxLen - 3 : 0;
    while (cut > 0 && ((unsigned char)r[cut] & 0xC0) == 0x80) --cut;
    r = r.substr(0, cut) + "...";
  }
  return r;
}
Status sanitizedFail(Err code, const std::string& rawDetail, bool recoverable) {
  return Status::Fail(code, sanitizeLibraryError(rawDetail), recoverable);
}

// ---------------- Paths ----------------
namespace {
Status bad(const std::string& why) { return Status::Fail(Err::InvalidArgument, "path rejected: " + why, true); }

// Returns false on invalid UTF-8; sets banned if a look-alike/invisible code point is present.
bool scanUtf8(const std::string& s, bool& banned) {
  banned = false;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i]; uint32_t cp; int len;
    if (c < 0x80) { ++i; continue; }
    if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
    else return false;
    if (i + len > s.size()) return false;
    for (int k = 1; k < len; ++k) { unsigned char d = (unsigned char)s[i + k]; if ((d & 0xC0) != 0x80) return false; cp = (cp << 6) | (d & 0x3F); }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    if (cp == 0x2024 || cp == 0x2025 || cp == 0x2044 || cp == 0x2215 || cp == 0x2216 || cp == 0xFE52 || cp == 0xFE68 || cp == 0xFF0E ||
        cp == 0xFF0F || cp == 0xFF3C || cp == 0xFEFF || cp == 0x29F5 || cp == 0x29F8 || cp == 0x2571 || cp == 0x2572 ||
        (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x00A0 || cp == 0x180E)
      banned = true;
    i += len;
  }
  return true;
}
bool reservedDeviceName(const std::string& seg) {
  std::string b = lower(seg.substr(0, seg.find('.')));
  while (!b.empty() && b.back() == ' ') b.pop_back();
  if (b == "con" || b == "prn" || b == "aux" || b == "nul" || b == "conin$" || b == "conout$" || b == "clock$") return true;
  if (b.size() == 4 && (b.compare(0, 3, "com") == 0 || b.compare(0, 3, "lpt") == 0) && b[3] >= '0' && b[3] <= '9') return true;
  return false;
}
bool splitSegs(const std::string& p, std::vector<std::string>& v) {
  v.clear(); size_t i = 0;
  while (true) { size_t j = i; while (j < p.size() && !isSep(p[j])) ++j; v.push_back(p.substr(i, j - i)); if (j >= p.size()) break; i = j + 1; }
  return true;
}
} // namespace

bool hasAllowedExtension(const std::string& rel, const std::vector<std::string>& exts) {
  if (exts.empty()) return true;
  size_t sl = rel.find_last_of("/\\"); std::string base = sl == std::string::npos ? rel : rel.substr(sl + 1);
  size_t d = base.find_last_of('.'); if (d == std::string::npos || d == 0) return false;
  std::string e = lower(base.substr(d));
  for (const auto& a : exts) if (e == a) return true;
  return false;
}

Status validateRelPath(const std::string& p, const RelPathPolicy& pol) {
  if (p.empty()) return bad("empty");
  if (p.size() > pol.maxLen) return bad("too long");
  for (unsigned char c : p) if (c < 0x20 || c == 0x7f) return bad("control character");
  bool banned = false;
  if (!scanUtf8(p, banned)) return bad("invalid UTF-8");
  if (banned) return bad("look-alike or invisible Unicode character");
  if (!urlScheme(p).empty() || p.find("://") != std::string::npos) return bad("scheme or network reference");
  if (isSep(p[0])) return bad("absolute or UNC path");
  if (p.size() >= 2 && isAlpha((unsigned char)p[0]) && p[1] == ':') return bad("drive path");
  if (p.find(':') != std::string::npos) return bad("':' not allowed (drive/stream)");
  for (char c : p) if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') return bad("illegal character");
  for (size_t i = 0; i + 2 < p.size() + 0; ++i) {
    if (p[i] == '%' && isHex((unsigned char)p[i + 1]) && isHex((unsigned char)p[i + 2])) {
      int v = std::stoi(p.substr(i + 1, 2), nullptr, 16);
      if (v == 0x2e || v == 0x2f || v == 0x5c || v == 0x3a || v < 0x20 || v == 0x7f) return bad("percent-encoded dot/separator");
    }
  }
  std::vector<std::string> segs; splitSegs(p, segs);
  if (segs.size() > pol.maxSegments) return bad("too many segments");
  size_t real = 0;
  for (size_t i = 0; i < segs.size(); ++i) {
    const std::string& g = segs[i];
    if (g.empty()) { if (!pol.allowRepeatedSeparators || i + 1 == segs.size()) return bad("empty segment"); continue; }
    if (g == ".") continue;
    if (g.size() > pol.maxSegmentLen) return bad("segment too long");
    if (g.find_first_not_of(". ") == std::string::npos) return bad("traversal ('..') segment");
    if (g.back() == '.' || g.back() == ' ') return bad("trailing dot or space");
    if (g[0] == ' ') return bad("leading space");
    if (reservedDeviceName(g)) return bad("reserved device name");
    ++real;
  }
  if (!real) return bad("no file name");
  if (!hasAllowedExtension(p, pol.allowedExt)) return bad("extension not allowed");
  return Status::Ok();
}

Status canonicalizeRelPath(const std::string& p, std::string& canonical, std::string& folded, const RelPathPolicy& pol) {
  Status s = validateRelPath(p, pol); if (!s.ok()) return s;
  std::vector<std::string> segs; splitSegs(p, segs);
  canonical.clear();
  for (auto& g : segs) { if (g.empty() || g == ".") continue; if (!canonical.empty()) canonical += '/'; canonical += g; }
  folded = lower(canonical);
  return Status::Ok();
}

Status CanonicalPathSet::add(const std::string& rel, std::string* out) {
  std::string c, f; Status s = canonicalizeRelPath(rel, c, f, pol_); if (!s.ok()) return s;
  if (keys_.size() >= max_) return Status::Fail(Err::ResourceExhausted, "archive entry count limit exceeded", false);
  if (!keys_.insert(f).second) return Status::Fail(Err::InvalidArgument, "duplicate canonical path", false);
  if (out) *out = c;
  return Status::Ok();
}

Status checkNoLinkEscape(const std::string& root, const std::string& rel, const ReparseProbe& probe) {
  namespace fs = std::filesystem;
  std::string c, f; Status s = canonicalizeRelPath(rel, c, f); if (!s.ok()) return s;
  if (root.empty()) return Status::Fail(Err::InvalidArgument, "empty root");
  std::error_code ec; fs::path cur(root);
  std::vector<std::string> segs; splitSegs(c, segs);
  for (auto& g : segs) {
    cur /= g;
    auto st = fs::symlink_status(cur, ec);
    if (ec || st.type() == fs::file_type::not_found) break;
    if (st.type() == fs::file_type::symlink) return Status::Fail(Err::PermissionDenied, "symlink in import path", false);
    if (probe && probe(cur.string())) return Status::Fail(Err::PermissionDenied, "reparse point in import path", false);
  }
  fs::path wr = fs::weakly_canonical(fs::path(root), ec); if (ec) return Status::Fail(Err::InvalidArgument, "root not resolvable");
  fs::path wc = fs::weakly_canonical(cur, ec); if (ec) return Status::Fail(Err::InvalidArgument, "path not resolvable");
  auto ri = wr.begin(), ci = wc.begin();
  for (; ri != wr.end(); ++ri, ++ci) if (ci == wc.end() || *ri != *ci) return Status::Fail(Err::PermissionDenied, "path escapes import root", false);
  return Status::Ok();
}

// ---------------- Protocols ----------------
std::string urlScheme(const std::string& s) {
  size_t i = 0; while (i < s.size() && ((unsigned char)s[i] <= 0x20 || (unsigned char)s[i] == 0x7f)) ++i;
  if (i >= s.size() || !isAlpha((unsigned char)s[i])) return "";
  size_t b = i; while (i < s.size() && isSchemeChar((unsigned char)s[i])) ++i;
  if (i >= s.size() || s[i] != ':' || i - b < 2) return "";
  return lower(s.substr(b, i - b));
}
bool isNetworkScheme(const std::string& sc) {
  static const char* k[] = {"http", "https", "ftp", "ftps", "sftp", "rtmp", "rtmps", "rtmpe", "rtmpt", "rtmpte", "rtmpts", "rtsp",
    "rtsps", "rtp", "srtp", "srt", "udp", "tcp", "tls", "ssl", "rist", "mms", "mmsh", "mmst", "gopher", "ws", "wss", "smb", "nfs",
    "ssh", "telnet", "ldap", "icecast", "dav", "davs", "webdav", "sctp", "zmq", "tftp", "ipfs", "sip", "hls", nullptr};
  std::string l = lower(sc);
  for (int i = 0; k[i]; ++i) if (l == k[i]) return true;
  return false;
}
Status checkLocalContentRef(const std::string& ref, const RelPathPolicy& pol) {
  std::string sc = urlScheme(ref);
  if (!sc.empty()) return Status::Fail(Err::InvalidArgument, isNetworkScheme(sc) ? "network protocol not allowed for local content" : "protocol not allowed for local content", false);
  if (ref.find("://") != std::string::npos) return Status::Fail(Err::InvalidArgument, "URL not allowed in local content", false);
  if (ref.find('|') != std::string::npos) return Status::Fail(Err::InvalidArgument, "composite/pipe reference not allowed", false);
  if (!ref.empty() && ref[0] == '-') return Status::Fail(Err::InvalidArgument, "reference may not start with '-'", false);
  return validateRelPath(ref, pol);
}
Status checkStreamUrlScheme(const std::string& url, bool allowRtp, bool allowInsecureRtmp) {
  std::string sc = urlScheme(url);
  if (sc.empty() || url.compare(url.find(':'), 3, "://") != 0) return Status::Fail(Err::InvalidArgument, "stream URL needs an explicit scheme://");
  if (sc == "rtmps" || sc == "srt") return Status::Ok();
  if (sc == "rtp" && allowRtp) return Status::Ok();
  if (sc == "rtmp" && allowInsecureRtmp) return Status::Ok();
  return Status::Fail(Err::UnsupportedCapability, "stream protocol '" + sanitizeLibraryError(sc, 16) + "' is not in the allowlist");
}
const std::vector<std::string>& localDecoderProtocolAllowlist() { static const std::vector<std::string> v = {"file"}; return v; }

bool isLoopbackOrDiscardEndpoint(const std::string& ep) {
  std::string sc = urlScheme(ep);
  if (sc == "discard" || sc == "null" || sc == "loopback") return ep.find('@') == std::string::npos;
  size_t p = ep.find("://");
  std::string rest = p == std::string::npos ? ep : ep.substr(p + 3);
  if (rest.find('@') != std::string::npos || rest.empty()) return false;
  std::string host;
  if (rest[0] == '[') { size_t e = rest.find(']'); if (e == std::string::npos) return false; host = rest.substr(1, e - 1); }
  else { size_t e = rest.find_first_of(":/?#"); host = rest.substr(0, e); }
  host = lower(host);
  if (host == "localhost" || host == "::1") return true;
  int parts = 0; size_t i = 0; int first = -1;
  while (i <= host.size()) {
    size_t j = host.find('.', i); if (j == std::string::npos) j = host.size();
    std::string o = host.substr(i, j - i);
    if (o.empty() || o.size() > 3) return false;
    int v = 0; for (char c : o) { if (!isDigit((unsigned char)c)) return false; v = v * 10 + (c - '0'); }
    if (v > 255) return false;
    if (parts == 0) first = v;
    ++parts; i = j + 1;
    if (j == host.size()) break;
  }
  return parts == 4 && first == 127;
}

Status checkCount(const char* what, uint64_t n, uint64_t limit) {
  if (n > limit) return Status::Fail(Err::ResourceExhausted, std::string(what) + " count exceeds limit", false);
  return Status::Ok();
}
Status checkDecompressed(uint64_t& total, uint64_t u, uint64_t c, const ImportLimits& lim) {
  if (u > lim.maxDecompressedBytes || total > lim.maxDecompressedBytes - u) return Status::Fail(Err::ResourceExhausted, "decompressed size limit exceeded", false);
  if (u > 0 && (c == 0 || u / c > lim.maxCompressionRatio)) return Status::Fail(Err::ResourceExhausted, "compression ratio too high (archive bomb)", false);
  total += u; return Status::Ok();
}

}} // namespace ss::sec
