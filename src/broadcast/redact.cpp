#include "broadcast/redact.h"
#include <cctype>
#include <vector>
namespace ss {
namespace cast {
static bool isUrlEnd(char c) { return std::isspace((unsigned char)c) || c == '"' || c == '\'' || c == ')' || c == ']' || c == '>' || c == '<' || c == ',' || c == ';' || c == '}'; }
static bool keyLikeSegment(const std::string& seg) {
  if (seg.size() >= 16) { size_t ok = 0; for (char c : seg) if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '=' || c == '.' || c == '+') ++ok; if (ok == seg.size()) return true; }
  std::string l; for (char c : seg) l.push_back((char)std::tolower((unsigned char)c));
  static const char* hints[] = {"key", "token", "secret", "pass", "auth", "sig"};
  for (auto h : hints) if (l.find(h) != std::string::npos && l.size() > 3) return true;
  return false;
}
std::string redactUrl(const std::string& url) {
  size_t sch = url.find("://");
  if (sch == std::string::npos) return url;
  std::string scheme = url.substr(0, sch);
  std::string rest = url.substr(sch + 3);
  std::string tail;   // query/fragment marker
  size_t q = rest.find_first_of("?#");
  bool hadQuery = q != std::string::npos;
  if (hadQuery) rest = rest.substr(0, q);
  size_t slash = rest.find('/');
  std::string auth = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string path = slash == std::string::npos ? "" : rest.substr(slash);
  size_t at = auth.rfind('@');
  if (at != std::string::npos) auth = auth.substr(at + 1);                // strip userinfo
  std::string outPath; bool rtmp = scheme == "rtmp" || scheme == "rtmps";
  size_t pos = 0; int segIndex = 0;
  while (pos < path.size()) {
    size_t nx = path.find('/', pos + 1); std::string seg = path.substr(pos + 1, nx == std::string::npos ? std::string::npos : nx - pos - 1);
    bool redact = (rtmp && segIndex >= 1) || keyLikeSegment(seg);
    outPath += "/" + (redact && !seg.empty() ? std::string("<redacted>") : seg);
    ++segIndex; if (nx == std::string::npos) break; pos = nx;
  }
  return scheme + "://" + auth + outPath + (hadQuery ? "?<redacted>" : "");
}
std::string redactText(const std::string& in) {
  std::string out; size_t i = 0;
  auto lower = [](char c) { return (char)std::tolower((unsigned char)c); };
  static const char* keys[] = {"streamkey", "stream_key", "stream-key", "passphrase", "password", "passwd", "secret", "token", "apikey", "api_key", "authorization", "key", "pwd", "signature"};
  while (i < in.size()) {
    // URL?
    size_t sch = in.find("://", i);
    size_t kwPos = std::string::npos; size_t kwLen = 0;
    // find the nearest secret-like key at or after i
    for (size_t j = i; j < in.size(); ++j) {
      if (!std::isalpha((unsigned char)in[j]) && in[j] != '_' && in[j] != '-') continue;
      if (j > i && (std::isalnum((unsigned char)in[j - 1]) )) continue;
      for (auto k : keys) {
        size_t n = std::char_traits<char>::length(k); bool m = j + n <= in.size();
        for (size_t t = 0; m && t < n; ++t) if (lower(in[j + t]) != k[t]) m = false;
        if (!m) continue;
        size_t e = j + n; while (e < in.size() && (in[e] == ' ' || in[e] == '\t')) ++e;
        if (e < in.size() && (in[e] == '=' || in[e] == ':') && !(in[e] == ':' && e + 2 < in.size() && in[e + 1] == '/' && in[e + 2] == '/')) { kwPos = j; kwLen = e + 1 - j; break; }
      }
      if (kwPos != std::string::npos) break;
    }
    size_t urlStart = std::string::npos;
    if (sch != std::string::npos) { size_t s = sch; while (s > i && (std::isalnum((unsigned char)in[s - 1]) || in[s - 1] == '+' || in[s - 1] == '.' || in[s - 1] == '-')) --s; urlStart = s; }
    if (urlStart == std::string::npos && kwPos == std::string::npos) { out.append(in, i, std::string::npos); break; }
    if (urlStart != std::string::npos && (kwPos == std::string::npos || urlStart <= kwPos)) {
      out.append(in, i, urlStart - i);
      size_t e = urlStart; while (e < in.size() && !isUrlEnd(in[e])) ++e;
      out += redactUrl(in.substr(urlStart, e - urlStart)); i = e;
    } else {
      out.append(in, i, kwPos + kwLen - i);
      size_t v = kwPos + kwLen; while (v < in.size() && (in[v] == ' ' || in[v] == '\t')) { out.push_back(in[v]); ++v; }
      size_t e = v; while (e < in.size() && !std::isspace((unsigned char)in[e]) && in[e] != '&' && in[e] != ',' && in[e] != ';' && in[e] != '"' && in[e] != '\'') ++e;
      if (e > v) out += "<redacted>";
      i = e;
    }
  }
  return out;
}
std::string redactWithSecret(const std::string& text, const std::string& secret) {
  std::string r = redactText(text);
  if (secret.size() >= 4) { size_t p = 0; while ((p = r.find(secret, p)) != std::string::npos) { r.replace(p, secret.size(), "<redacted>"); p += 10; } }
  return r;
}
void wipe(std::string& s) { volatile char* p = s.empty() ? nullptr : &s[0]; for (size_t i = 0; i < s.size(); ++i) p[i] = 0; s.clear(); }
} // namespace cast
} // namespace ss
