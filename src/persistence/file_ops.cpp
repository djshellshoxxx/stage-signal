#include "persistence/file_ops.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#else
extern "C" int _commit(int);   // CRT symbol; declared here to keep Windows headers out of portable code
#endif

namespace fs = std::filesystem;

namespace ss {
namespace persist {
namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }

Status errStatus(const std::error_code& ec, const std::string& what) {
  Err c = Err::InternalFault;
  if (ec == std::errc::no_space_on_device) c = Err::DiskFull;
  else if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted || ec == std::errc::read_only_file_system) c = Err::PermissionDenied;
  else if (ec == std::errc::no_such_file_or_directory) c = Err::InvalidArgument;
  return Status::Fail(c, what + ": " + ec.message());
}
Status errnoStatus(int e, const std::string& what) { return errStatus(std::error_code(e, std::generic_category()), what); }

std::FILE* openFile(const std::string& path, bool write) {
#ifdef _WIN32
  return _wfopen(P(path).c_str(), write ? L"wb" : L"rb");
#else
  return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

struct StdWrite : IWriteFile {
  std::FILE* f; std::string path;
  StdWrite(std::FILE* ff, std::string p) : f(ff), path(std::move(p)) {}
  ~StdWrite() override { if (f) std::fclose(f); }
  Status write(const void* d, size_t n) override {
    if (!f) return Status::Fail(Err::InternalFault, "write on closed file");
    if (n && std::fwrite(d, 1, n, f) != n) return errnoStatus(errno ? errno : EIO, "write " + path);
    return Status::Ok();
  }
  Status flushAndClose() override {
    if (!f) return Status::Ok();
    int e = 0;
    if (std::fflush(f) != 0) e = errno;
#ifdef _WIN32
    else if (_commit(_fileno(f)) != 0) e = errno;
#else
    else if (fsync(fileno(f)) != 0) e = errno;
#endif
    if (std::fclose(f) != 0 && !e) e = errno;
    f = nullptr;
    return e ? errnoStatus(e, "flush " + path) : Status::Ok();
  }
};
struct StdRead : IReadFile {
  std::FILE* f; uint64_t sz = 0;
  explicit StdRead(std::FILE* ff) : f(ff) {
    if (f) { std::fseek(f, 0, SEEK_END); long long e = (long long)std::ftell(f); sz = e < 0 ? 0 : (uint64_t)e; std::fseek(f, 0, SEEK_SET); }
  }
  ~StdRead() override { if (f) std::fclose(f); }
  uint64_t size() const override { return sz; }
  Status seek(uint64_t off) override {
    if (off > sz) return Status::Fail(Err::InvalidArgument, "seek past end");
    return std::fseek(f, (long)off, SEEK_SET) == 0 ? Status::Ok() : errnoStatus(errno, "seek");
  }
  Status read(void* d, size_t n, size_t& got) override {
    got = n ? std::fread(d, 1, n, f) : 0;
    if (got < n && std::ferror(f)) return errnoStatus(errno ? errno : EIO, "read");
    return Status::Ok();
  }
};

struct StdOps : IFileOps {
  Status openWrite(const std::string& path, std::unique_ptr<IWriteFile>& out) override {
    std::FILE* f = openFile(path, true);
    if (!f) return errnoStatus(errno ? errno : EIO, "open for write " + path);
    out.reset(new StdWrite(f, path));
    return Status::Ok();
  }
  Status openRead(const std::string& path, std::unique_ptr<IReadFile>& out) override {
    std::FILE* f = openFile(path, false);
    if (!f) return Status::Fail(Err::InvalidArgument, "cannot open " + fileName(path));
    out.reset(new StdRead(f));
    return Status::Ok();
  }
  Status rename(const std::string& from, const std::string& to) override {
    std::error_code ec;
    fs::rename(P(from), P(to), ec);
#ifdef _WIN32
    if (ec && fs::exists(P(to)) && !fs::is_directory(P(to))) { std::error_code e2; fs::remove(P(to), e2); ec.clear(); fs::rename(P(from), P(to), ec); }
#endif
    return ec ? errStatus(ec, "rename") : Status::Ok();
  }
  Status remove(const std::string& path) override {
    std::error_code ec; fs::remove(P(path), ec);
    return ec ? errStatus(ec, "remove") : Status::Ok();
  }
  Status removeAll(const std::string& path) override {
    std::error_code ec; fs::remove_all(P(path), ec);
    return ec ? errStatus(ec, "remove_all") : Status::Ok();
  }
  Status createDirs(const std::string& path) override {
    std::error_code ec; fs::create_directories(P(path), ec);
    return ec ? errStatus(ec, "mkdir") : Status::Ok();
  }
  Status createPrivateDir(const std::string& path) override {
    std::error_code ec;
    bool made = fs::create_directory(P(path), ec);
    if (ec) return errStatus(ec, "mkdir");
    if (!made) return Status::Fail(Err::InvalidArgument, "directory already exists");
    fs::permissions(P(path), fs::perms::owner_all, fs::perm_options::replace, ec);   // best effort
    return Status::Ok();
  }
  bool exists(const std::string& path) override { std::error_code ec; return fs::exists(fs::symlink_status(P(path), ec)); }
  bool isSymlink(const std::string& path) override { std::error_code ec; return fs::is_symlink(fs::symlink_status(P(path), ec)); }
  Status listDir(const std::string& path, std::vector<std::string>& names) override {
    names.clear();
    std::error_code ec;
    for (fs::directory_iterator it(P(path), ec), end; !ec && it != end; it.increment(ec)) names.push_back(it->path().filename().u8string());
    return ec ? errStatus(ec, "list") : Status::Ok();
  }
};

struct FaultWrite : IWriteFile {
  std::unique_ptr<IWriteFile> inner; FaultFileOps* o; bool applies;
  FaultWrite(std::unique_ptr<IWriteFile> i, FaultFileOps* ops, bool ap) : inner(std::move(i)), o(ops), applies(ap) {}
  Status write(const void* d, size_t n) override {
    if (applies && o->crashAfterBytes >= 0 && o->bytesWritten + (int64_t)n > o->crashAfterBytes) {
      size_t keep = (size_t)(o->crashAfterBytes - o->bytesWritten);
      if (keep) inner->write(d, keep);
      o->bytesWritten += (int64_t)keep;
      inner->flushAndClose();            // the partial bytes really reached the disk
      o->dead = true;
      return Status::Fail(Err::InternalFault, "injected crash during write");
    }
    o->bytesWritten += (int64_t)n;
    return inner->write(d, n);
  }
  Status flushAndClose() override {
    if (applies && o->failFlush) { inner->flushAndClose(); return Status::Fail(Err::DiskFull, "injected flush failure"); }
    return inner->flushAndClose();
  }
};

} // namespace

IFileOps& defaultFileOps() { static StdOps o; return o; }

std::string parentDir(const std::string& path) {
  size_t p = path.find_last_of("/\\");
  if (p == std::string::npos) return ".";
  if (p == 0) return path.substr(0, 1);
  return path.substr(0, p);
}
std::string fileName(const std::string& path) {
  size_t p = path.find_last_of("/\\");
  return p == std::string::npos ? path : path.substr(p + 1);
}
std::string joinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  char last = a.back();
  return (last == '/' || last == '\\') ? a + b : a + "/" + b;
}

Status writeFileAll(IFileOps& ops, const std::string& path, const void* d, size_t n) {
  std::unique_ptr<IWriteFile> w;
  Status s = ops.openWrite(path, w);
  if (!s.ok()) return s;
  s = w->write(d, n);
  if (!s.ok()) return s;
  return w->flushAndClose();
}

Status readFileAll(IFileOps& ops, const std::string& path, size_t maxBytes, std::vector<uint8_t>& out) {
  std::unique_ptr<IReadFile> r;
  Status s = ops.openRead(path, r);
  if (!s.ok()) return s;
  if (r->size() > maxBytes) return Status::Fail(Err::ResourceExhausted, "file larger than allowed limit");   // before allocation
  out.assign((size_t)r->size(), 0);
  size_t got = 0;
  s = r->read(out.data(), out.size(), got);
  if (!s.ok()) return s;
  if (got != out.size()) return Status::Fail(Err::InternalFault, "short read");
  return Status::Ok();
}

Status copyFile(IFileOps& ops, const std::string& from, const std::string& to) {
  std::unique_ptr<IReadFile> r; Status s = ops.openRead(from, r);
  if (!s.ok()) return s;
  std::unique_ptr<IWriteFile> w; s = ops.openWrite(to, w);
  if (!s.ok()) return s;
  std::vector<uint8_t> buf(64 * 1024);
  for (;;) {
    size_t got = 0; s = r->read(buf.data(), buf.size(), got);
    if (!s.ok()) return s;
    if (got == 0) break;
    s = w->write(buf.data(), got);
    if (!s.ok()) return s;
    if (got < buf.size()) break;
  }
  return w->flushAndClose();
}

Status FaultFileOps::openWrite(const std::string& path, std::unique_ptr<IWriteFile>& out) {
  if (dead) return deadStatus();
  ++opens; writtenPaths.push_back(path);
  std::unique_ptr<IWriteFile> inner;
  Status s = base_.openWrite(path, inner);
  if (!s.ok()) return s;
  bool applies = onlyPathContaining.empty() || path.find(onlyPathContaining) != std::string::npos;
  out.reset(new FaultWrite(std::move(inner), this, applies));
  return Status::Ok();
}
Status FaultFileOps::rename(const std::string& from, const std::string& to) {
  if (dead) return deadStatus();
  ++renames;
  if (failRenameNth > 0 && renames == failRenameNth) return Status::Fail(Err::InternalFault, "injected rename failure");
  return base_.rename(from, to);
}

} // namespace persist
} // namespace ss
