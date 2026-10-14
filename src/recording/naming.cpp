#include "recording/naming.h"
#include <cstdio>
#include <cctype>
#include <algorithm>
namespace ss {
namespace rec {
static bool reservedWin(const std::string& s) {
  std::string u; for (char c : s) { if (c == '.') break; u.push_back((char)std::toupper((unsigned char)c)); }
  static const char* names[] = {"CON", "PRN", "AUX", "NUL", "CLOCK$"};
  for (auto n : names) if (u == n) return true;
  if (u.size() == 4 && (u.compare(0, 3, "COM") == 0 || u.compare(0, 3, "LPT") == 0) && u[3] >= '0' && u[3] <= '9') return true;
  return false;
}
std::string sanitizeShowName(const std::string& in) {
  std::string o; bool lastUnd = true;
  for (unsigned char c : in) {
    bool ok = (c < 128) && (std::isalnum(c) || c == '.' || c == '-' || c == '_');
    if (ok && c != '_') { o.push_back((char)c); lastUnd = false; }
    else { if (!lastUnd) o.push_back('_'); lastUnd = true; }
  }
  for (size_t k; (k = o.find("..")) != std::string::npos;) o.replace(k, 2, "_");
  for (size_t k; (k = o.find("__")) != std::string::npos;) o.erase(k, 1);
  while (!o.empty() && (o.back() == '_' || o.back() == '.')) o.pop_back();
  while (!o.empty() && (o.front() == '.' || o.front() == '_')) o.erase(o.begin());
  if (o.size() > 40) o.resize(40);
  while (!o.empty() && (o.back() == '_' || o.back() == '.')) o.pop_back();
  if (o.empty()) o = "show";
  if (reservedWin(o)) o = "_" + o;
  return o;
}
std::string sanitizeSessionId(const std::string& in) {
  std::string o;
  for (unsigned char c : in) { if (c < 128 && (std::isalnum(c) || c == '-')) o.push_back((char)c); else if (!o.empty() && o.back() != '-') o.push_back('-'); }
  if (o.size() > 24) o.resize(24);
  if (o.empty()) o = "S";
  return o;
}
std::string utcStamp(int64_t unixMs) {
  int64_t s = unixMs / 1000; if (unixMs < 0 && unixMs % 1000) --s;
  int64_t days = s / 86400, rem = s % 86400; if (rem < 0) { rem += 86400; --days; }
  // civil_from_days (H. Hinnant)
  int64_t z = days + 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097; int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400; int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); int64_t mp = (5 * doy + 2) / 153;
  int64_t d = doy - (153 * mp + 2) / 5 + 1; int64_t m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) ++y;
  char b[32];
  std::snprintf(b, sizeof b, "%04d%02d%02dT%02d%02d%02dZ", (int)y, (int)m, (int)d, (int)(rem / 3600), (int)(rem % 3600 / 60), (int)(rem % 60));
  return b;
}
std::string baseName(int64_t unixMs, const std::string& sessionId, const std::string& showName) {
  return "StageSignal_" + utcStamp(unixMs) + "_" + sanitizeSessionId(sessionId) + "_" + sanitizeShowName(showName);
}
std::string segmentName(const std::string& base, int index, const char* ext) {
  char b[16]; std::snprintf(b, sizeof b, "_%04d.", index); return base + b + ext;
}
std::string joinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  char c = dir.back(); return (c == '/' || c == '\\') ? dir + name : dir + "/" + name;
}
} // namespace rec
} // namespace ss
