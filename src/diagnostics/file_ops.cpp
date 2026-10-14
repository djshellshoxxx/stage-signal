#include "diagnostics/file_ops.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>

namespace ss {
namespace fs = std::filesystem;

std::string joinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  char c = a.back(); return (c == '/' || c == '\\') ? a + b : a + "/" + b;
}

namespace {
struct StdOps : IFileOps {
  bool createDirs(const std::string& d) override { std::error_code ec; fs::create_directories(fs::path(d), ec); return !ec && fs::is_directory(fs::path(d), ec); }
  bool append(const std::string& p, const char* d, size_t n) override {
    std::ofstream f(p, std::ios::binary | std::ios::app); if (!f) return false;
    f.write(d, (std::streamsize)n); f.flush(); return (bool)f;
  }
  bool writeAll(const std::string& p, const std::string& data) override {
    std::ofstream f(p, std::ios::binary | std::ios::trunc); if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size()); f.flush(); return (bool)f;
  }
  bool readAll(const std::string& p, std::string& out, size_t maxBytes) override {
    std::error_code ec; auto sz = fs::file_size(fs::path(p), ec); if (ec || sz > maxBytes) return false;
    std::ifstream f(p, std::ios::binary); if (!f) return false;
    out.resize((size_t)sz); if (sz) f.read(&out[0], (std::streamsize)sz);
    return (bool)f || f.eof();
  }
  int64_t fileSize(const std::string& p) override { std::error_code ec; auto s = fs::file_size(fs::path(p), ec); return ec ? -1 : (int64_t)s; }
  int64_t mtimeUnixMs(const std::string& p) override {
    std::error_code ec; auto t = fs::last_write_time(fs::path(p), ec); if (ec) return -1;
    // portable file_clock -> system_clock approximation (C++17 has no clock_cast)
    auto nowF = fs::file_time_type::clock::now(); auto nowS = std::chrono::system_clock::now();
    auto d = std::chrono::duration_cast<std::chrono::milliseconds>(nowS.time_since_epoch()) - std::chrono::duration_cast<std::chrono::milliseconds>(nowF - t);
    return (int64_t)d.count();
  }
  bool rename(const std::string& a, const std::string& b) override {
    std::error_code ec; fs::rename(fs::path(a), fs::path(b), ec);
    if (ec) { fs::remove(fs::path(b), ec); ec.clear(); fs::rename(fs::path(a), fs::path(b), ec); }
    return !ec;
  }
  bool remove(const std::string& p) override { std::error_code ec; fs::remove(fs::path(p), ec); return !fs::exists(fs::path(p), ec); }
  std::vector<std::string> listDir(const std::string& d) override {
    std::vector<std::string> v; std::error_code ec;
    for (fs::directory_iterator it(fs::path(d), ec), e; !ec && it != e; it.increment(ec)) {
      std::error_code e2; if (it->is_regular_file(e2)) v.push_back(it->path().filename().string());
    }
    std::sort(v.begin(), v.end()); return v;
  }
};
} // namespace

IFileOps& stdFileOps() { static StdOps o; return o; }

bool MemFileOps::append(const std::string& p, const char* d, size_t n) {
  if (failAll || totalBytes() + n > quotaBytes) return false;
  auto& e = files[p]; e.data.append(d, n); e.mtime = nowMs; return true;
}
bool MemFileOps::writeAll(const std::string& p, const std::string& data) {
  if (failAll) return false;
  uint64_t cur = totalBytes() - (files.count(p) ? files[p].data.size() : 0);
  if (cur + data.size() > quotaBytes) return false;
  auto& e = files[p]; e.data = data; e.mtime = nowMs; return true;
}
bool MemFileOps::readAll(const std::string& p, std::string& out, size_t maxBytes) {
  auto i = files.find(p); if (failAll || i == files.end() || i->second.data.size() > maxBytes) return false;
  out = i->second.data; return true;
}
bool MemFileOps::rename(const std::string& a, const std::string& b) {
  if (failAll) return false;
  auto i = files.find(a); if (i == files.end()) return false;
  Entry e = i->second; files.erase(i); files[b] = e; return true;
}
std::vector<std::string> MemFileOps::listDir(const std::string& dir) {
  std::vector<std::string> v; std::string pre = dir.empty() ? "" : (dir.back() == '/' ? dir : dir + "/");
  for (auto& f : files) if (f.first.compare(0, pre.size(), pre) == 0 && f.first.find('/', pre.size()) == std::string::npos) v.push_back(f.first.substr(pre.size()));
  return v;
}

} // namespace ss
