#include "recording/io.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#if defined(_WIN32)
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#endif

namespace ss {
namespace {
int seek64(FILE* f, uint64_t off, int whence) {
#if defined(_WIN32)
  return _fseeki64(f, (long long)off, whence);
#else
  return fseeko(f, (off_t)off, whence);
#endif
}
int64_t tell64(FILE* f) {
#if defined(_WIN32)
  return _ftelli64(f);
#else
  return (int64_t)ftello(f);
#endif
}
class StdSink : public IFileSink {
 public:
  explicit StdSink(FILE* f) : f_(f) { seek64(f_, 0, SEEK_END); int64_t t = tell64(f_); size_ = t < 0 ? 0 : (uint64_t)t; }
  ~StdSink() override { if (f_) std::fclose(f_); }
  Status write(const void* d, size_t n) override {
    if (!f_) return Status::Fail(Err::InternalFault, "file closed");
    if (n && std::fwrite(d, 1, n, f_) != n) return Status::Fail(Err::DiskFull, "write failed (disk full or I/O error)", false);
    size_ += n; return Status::Ok();
  }
  Status writeAt(uint64_t off, const void* d, size_t n) override {
    if (!f_) return Status::Fail(Err::InternalFault, "file closed");
    std::fflush(f_);
    if (seek64(f_, off, SEEK_SET) != 0) return Status::Fail(Err::InternalFault, "seek failed");
    size_t w = n ? std::fwrite(d, 1, n, f_) : 0;
    std::fflush(f_);
    seek64(f_, 0, SEEK_END);
    if (w != n) return Status::Fail(Err::DiskFull, "patch write failed", false);
    return Status::Ok();
  }
  Status flush() override { if (f_ && std::fflush(f_) != 0) return Status::Fail(Err::DiskFull, "flush failed", false); return Status::Ok(); }
  Status close() override {
    if (!f_) return Status::Ok();
    int r = std::fclose(f_); f_ = nullptr;
    return r == 0 ? Status::Ok() : Status::Fail(Err::DiskFull, "close failed", false);
  }
  uint64_t size() const override { return size_; }
 private:
  FILE* f_; uint64_t size_ = 0;
};
class StdFs : public IFileSystem {
 public:
  Status open(const std::string& p, Mode m, std::unique_ptr<IFileSink>& out) override {
    const char* mode = m == Mode::Truncate ? "wb" : (m == Mode::Append ? "ab" : "r+b");
    FILE* f = std::fopen(p.c_str(), mode);
    if (!f) return Status::Fail(Err::PermissionDenied, "cannot open file for writing: " + p);
    out.reset(new StdSink(f)); return Status::Ok();
  }
  Status makeDirs(const std::string& dir) override {
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
      if (i == dir.size() || dir[i] == '/' || dir[i] == '\\') {
        if (!cur.empty() && cur != "." && !(cur.size() == 2 && cur[1] == ':')) {
#if defined(_WIN32)
          _mkdir(cur.c_str());
#else
          ::mkdir(cur.c_str(), 0755);
#endif
        }
      }
      if (i < dir.size()) cur.push_back(dir[i]);
    }
    return Status::Ok();
  }
  bool exists(const std::string& p) override { FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return false; std::fclose(f); return true; }
  bool fileSize(const std::string& p, uint64_t& out) override {
    FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return false;
    seek64(f, 0, SEEK_END); int64_t t = tell64(f); std::fclose(f);
    if (t < 0) return false;
    out = (uint64_t)t; return true;
  }
  bool readAll(const std::string& p, rec::Bytes& out) override {
    FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return false;
    out.clear(); uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f); return true;
  }
  Status resize(const std::string& p, uint64_t size) override {
#if defined(_WIN32)
    FILE* f = std::fopen(p.c_str(), "r+b"); if (!f) return Status::Fail(Err::PermissionDenied, "cannot open for resize");
    int r = _chsize_s(_fileno(f), (long long)size); std::fclose(f);
    return r == 0 ? Status::Ok() : Status::Fail(Err::InternalFault, "resize failed");
#else
    return ::truncate(p.c_str(), (off_t)size) == 0 ? Status::Ok() : Status::Fail(Err::InternalFault, "resize failed");
#endif
  }
  Status rename(const std::string& a, const std::string& b) override {
    std::remove(b.c_str());
    return std::rename(a.c_str(), b.c_str()) == 0 ? Status::Ok() : Status::Fail(Err::InternalFault, "rename failed");
  }
  Status remove(const std::string& p) override { return std::remove(p.c_str()) == 0 ? Status::Ok() : Status::Fail(Err::InternalFault, "remove failed"); }
  bool writableDir(const std::string& dir, std::string& why) override {
    makeDirs(dir);
    std::string probe = dir + "/.ss_probe.tmp";
    FILE* f = std::fopen(probe.c_str(), "wb");
    if (!f) { why = "folder is not writable or does not exist: " + dir; return false; }
    std::fclose(f); std::remove(probe.c_str()); return true;
  }
};

class MemSink : public IFileSink {
 public:
  MemSink(MemFileSystem* fs, std::shared_ptr<MemFileSystem::File> f) : fs_(fs), f_(std::move(f)) {}
  Status write(const void* d, size_t n) override {
    std::function<void(size_t)> hook;
    { std::lock_guard<std::mutex> l(fs_->mu);
      if (closed_) return Status::Fail(Err::InternalFault, "file closed");
      if (fs_->plan.failWriteAfterBytes >= 0 && (int64_t)fs_->written >= fs_->plan.failWriteAfterBytes)
        return Status::Fail(Err::InternalFault, "I/O error (injected)", false);
      if (fs_->written + n > fs_->plan.capacityBytes) {
        // partial write like a real full disk: accept what fits
        size_t fit = (size_t)(fs_->plan.capacityBytes > fs_->written ? fs_->plan.capacityBytes - fs_->written : 0);
        f_->data.insert(f_->data.end(), (const uint8_t*)d, (const uint8_t*)d + fit);
        fs_->written += fit; ++fs_->writes_;
        return Status::Fail(Err::DiskFull, "disk full (injected)", false);
      }
      f_->data.insert(f_->data.end(), (const uint8_t*)d, (const uint8_t*)d + n);
      fs_->written += n; ++fs_->writes_;
      hook = fs_->plan.onWrite;
    }
    if (hook) hook(n);
    return Status::Ok();
  }
  Status writeAt(uint64_t off, const void* d, size_t n) override {
    std::lock_guard<std::mutex> l(fs_->mu);
    if (fs_->plan.failPatch) return Status::Fail(Err::InternalFault, "patch failed (injected)", false);
    if (off + n > f_->data.size()) return Status::Fail(Err::InvalidArgument, "patch beyond end");
    std::memcpy(f_->data.data() + off, d, n); return Status::Ok();
  }
  Status flush() override { return Status::Ok(); }
  Status close() override { std::lock_guard<std::mutex> l(fs_->mu); closed_ = true; return Status::Ok(); }
  uint64_t size() const override { std::lock_guard<std::mutex> l(fs_->mu); return f_->data.size(); }
 private:
  MemFileSystem* fs_; std::shared_ptr<MemFileSystem::File> f_; bool closed_ = false;
};
} // namespace

IFileSystem& stdFileSystem() { static StdFs fs; return fs; }

#if !defined(_WIN32)
namespace {
struct PosixDisk : IDiskInfo {
  bool freeBytes(const std::string& path, uint64_t& out) override {
    struct statvfs s;
    if (statvfs(path.empty() ? "." : path.c_str(), &s) != 0) return false;
    out = (uint64_t)s.f_bavail * (uint64_t)s.f_frsize; return true;
  }
};
}
IDiskInfo& posixDiskInfo() { static PosixDisk d; return d; }
#endif

Status MemFileSystem::open(const std::string& path, Mode m, std::unique_ptr<IFileSink>& out) {
  std::shared_ptr<File> f;
  { std::lock_guard<std::mutex> l(mu);
    if (plan.failOpen) return Status::Fail(Err::PermissionDenied, "open failed (injected)");
    auto it = files.find(path);
    if (m == Mode::Truncate || it == files.end()) {
      if (m == Mode::Update && it == files.end()) return Status::Fail(Err::PermissionDenied, "no such file");
      f = std::make_shared<File>(); files[path] = f;
    } else f = it->second;
  }
  out.reset(new MemSink(this, f)); return Status::Ok();
}
bool MemFileSystem::exists(const std::string& p) { std::lock_guard<std::mutex> l(mu); return files.count(p) != 0; }
bool MemFileSystem::fileSize(const std::string& p, uint64_t& out) { std::lock_guard<std::mutex> l(mu); auto it = files.find(p); if (it == files.end()) return false; out = it->second->data.size(); return true; }
bool MemFileSystem::readAll(const std::string& p, rec::Bytes& out) { std::lock_guard<std::mutex> l(mu); auto it = files.find(p); if (it == files.end()) return false; out = it->second->data; return true; }
Status MemFileSystem::resize(const std::string& p, uint64_t size) { std::lock_guard<std::mutex> l(mu); auto it = files.find(p); if (it == files.end()) return Status::Fail(Err::InvalidArgument, "no such file"); it->second->data.resize((size_t)size); return Status::Ok(); }
Status MemFileSystem::rename(const std::string& a, const std::string& b) { std::lock_guard<std::mutex> l(mu); auto it = files.find(a); if (it == files.end()) return Status::Fail(Err::InvalidArgument, "no such file"); files[b] = it->second; files.erase(a); return Status::Ok(); }
Status MemFileSystem::remove(const std::string& p) { std::lock_guard<std::mutex> l(mu); files.erase(p); return Status::Ok(); }
std::vector<std::string> MemFileSystem::list() const { std::vector<std::string> v; auto* self = const_cast<MemFileSystem*>(this); std::lock_guard<std::mutex> l(self->mu); for (auto& kv : files) v.push_back(kv.first); return v; }
uint64_t MemFileSystem::totalBytes() const { auto* self = const_cast<MemFileSystem*>(this); std::lock_guard<std::mutex> l(self->mu); uint64_t t = 0; for (auto& kv : files) t += kv.second->data.size(); return t; }
void MemFileSystem::chop(const std::string& p, size_t n) { std::lock_guard<std::mutex> l(mu); auto it = files.find(p); if (it == files.end()) return; auto& d = it->second->data; d.resize(d.size() > n ? d.size() - n : 0); }

} // namespace ss
