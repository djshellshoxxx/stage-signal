#include "ss/persist/fs.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace ss::persist {
namespace fsys = std::filesystem;

std::string parentDir(const std::string& path) {
    const auto p = path.find_last_of('/');
    return p == std::string::npos ? std::string() : path.substr(0, p);
}
std::string baseName(const std::string& path) {
    const auto p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}
std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

namespace {
Error ioError(const std::error_code& ec) {
    if (ec == std::errc::no_space_on_device) return makeError(ErrorCategory::DiskFull, "disk full", Recoverability::OperatorAction);
    if (ec == std::errc::permission_denied || ec == std::errc::read_only_file_system)
        return makeError(ErrorCategory::PermissionDenied, "permission denied", Recoverability::OperatorAction);
    if (ec == std::errc::no_such_file_or_directory) return makeError(ErrorCategory::InvalidArgument, "file not found", Recoverability::OperatorAction);
    return makeError(ErrorCategory::InternalFault, "filesystem error", Recoverability::Retryable);
}
fsys::path toPath(const std::string& p) { return fsys::u8path(p); }
}  // namespace

// ---- StdFileSystem ------------------------------------------------------------------------------
Result<std::string> StdFileSystem::readFile(const std::string& path, std::uint64_t maxBytes) {
    std::error_code ec;
    const auto size = fsys::file_size(toPath(path), ec);
    if (ec) return ioError(ec);
    if (size > maxBytes) return makeError(ErrorCategory::ResourceExhausted, "file larger than allowed");
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in) return makeError(ErrorCategory::PermissionDenied, "cannot open file");
    std::string data(static_cast<std::size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(size));
    if (static_cast<std::uint64_t>(in.gcount()) != size) return makeError(ErrorCategory::InternalFault, "short read", Recoverability::Retryable);
    return data;
}

Status StdFileSystem::writeFile(const std::string& path, const std::string& data, bool flush) {
    std::ofstream out(toPath(path), std::ios::binary | std::ios::trunc);
    if (!out) return makeError(ErrorCategory::PermissionDenied, "cannot create file");
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (flush) out.flush();
    if (!out) {
        const std::error_code ec(errno, std::generic_category());
        return ioError(ec);
    }
    out.close();
    if (out.fail()) return makeError(ErrorCategory::DiskFull, "write failed", Recoverability::OperatorAction);
    return okStatus();
}

Status StdFileSystem::rename(const std::string& from, const std::string& to) {
    std::error_code ec;
    fsys::rename(toPath(from), toPath(to), ec);
    if (ec) return ioError(ec);
    return okStatus();
}

Status StdFileSystem::remove(const std::string& path) {
    std::error_code ec;
    fsys::remove(toPath(path), ec);
    if (ec) return ioError(ec);
    return okStatus();
}

bool StdFileSystem::exists(const std::string& path) {
    std::error_code ec;
    return fsys::exists(toPath(path), ec) && !ec;
}

Result<std::vector<std::string>> StdFileSystem::listDir(const std::string& dir) {
    std::error_code ec;
    std::vector<std::string> names;
    fsys::directory_iterator it(toPath(dir), ec), end;
    if (ec) return ioError(ec);
    for (; it != end; it.increment(ec)) {
        if (ec) return ioError(ec);
        std::error_code e2;
        if (it->is_regular_file(e2)) names.push_back(it->path().filename().u8string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

Status StdFileSystem::createDirs(const std::string& dir) {
    std::error_code ec;
    fsys::create_directories(toPath(dir), ec);
    if (ec) return ioError(ec);
    return okStatus();
}

// ---- MemFileSystem ------------------------------------------------------------------------------
namespace {
Error crashedError() { return makeError(ErrorCategory::InternalFault, "simulated crash", Recoverability::Fatal); }
}

bool MemFileSystem::parentExists(const std::string& path) const {
    const std::string d = parentDir(path);
    return d.empty() || dirs_.count(d) != 0;
}

void MemFileSystem::crash() {
    crashed_ = true;
    if (loseUnflushedOnCrash)
        for (const auto& p : unflushed_) { auto it = files_.find(p); if (it != files_.end()) it->second.clear(); }
    unflushed_.clear();
}

Result<std::string> MemFileSystem::readFile(const std::string& path, std::uint64_t maxBytes) {
    if (crashed_) return crashedError();
    auto it = files_.find(path);
    if (it == files_.end()) return makeError(ErrorCategory::InvalidArgument, "file not found");
    if (it->second.size() > maxBytes) return makeError(ErrorCategory::ResourceExhausted, "file larger than allowed");
    return it->second;
}

Status MemFileSystem::writeFile(const std::string& path, const std::string& data, bool flush) {
    if (crashed_) return crashedError();
    ++ops_;
    if (!parentExists(path)) return makeError(ErrorCategory::InvalidArgument, "directory not found");
    if (hasFault_ && ops_ == fault_.atOp) {
        const auto n = static_cast<std::size_t>(static_cast<double>(data.size()) * fault_.tornFraction);
        files_[path] = data.substr(0, n);
        if (flush) unflushed_.erase(path); else unflushed_.insert(path);   // torn bytes of a flushed write stay on disk
        if (fault_.kind == FaultKind::Crash) { crash(); return crashedError(); }
        return makeError(ErrorCategory::DiskFull, "disk full", Recoverability::OperatorAction);
    }
    files_[path] = data;
    if (flush) unflushed_.erase(path); else unflushed_.insert(path);
    return okStatus();
}

Status MemFileSystem::rename(const std::string& from, const std::string& to) {
    if (crashed_) return crashedError();
    ++ops_;
    if (hasFault_ && ops_ == fault_.atOp) {
        if (fault_.kind == FaultKind::Crash) { crash(); return crashedError(); }
        return makeError(ErrorCategory::DiskFull, "disk full", Recoverability::OperatorAction);
    }
    auto it = files_.find(from);
    if (it == files_.end()) return makeError(ErrorCategory::InvalidArgument, "file not found");
    if (!parentExists(to)) return makeError(ErrorCategory::InvalidArgument, "directory not found");
    const bool dirty = unflushed_.count(from) != 0;
    files_[to] = std::move(it->second);
    files_.erase(from);
    unflushed_.erase(from);
    unflushed_.erase(to);
    if (dirty) unflushed_.insert(to);
    return okStatus();
}

Status MemFileSystem::remove(const std::string& path) {
    if (crashed_) return crashedError();
    ++ops_;
    if (hasFault_ && ops_ == fault_.atOp) {
        if (fault_.kind == FaultKind::Crash) { crash(); return crashedError(); }
        return makeError(ErrorCategory::InternalFault, "remove failed", Recoverability::Retryable);
    }
    files_.erase(path);
    unflushed_.erase(path);
    return okStatus();
}

bool MemFileSystem::exists(const std::string& path) { return !crashed_ && files_.count(path) != 0; }

Result<std::vector<std::string>> MemFileSystem::listDir(const std::string& dir) {
    if (crashed_) return crashedError();
    if (!dir.empty() && dirs_.count(dir) == 0) return makeError(ErrorCategory::InvalidArgument, "directory not found");
    std::vector<std::string> names;
    for (const auto& kv : files_)
        if (parentDir(kv.first) == dir) names.push_back(baseName(kv.first));
    return names;   // std::map order is already sorted
}

Status MemFileSystem::createDirs(const std::string& dir) {
    if (crashed_) return crashedError();
    std::string cur;
    std::size_t i = 0;
    while (i <= dir.size()) {
        std::size_t j = dir.find('/', i);
        if (j == std::string::npos) j = dir.size();
        if (j > i || i == 0) { cur = dir.substr(0, j); if (!cur.empty()) dirs_.insert(cur); }
        i = j + 1;
    }
    return okStatus();
}

void MemFileSystem::put(const std::string& path, const std::string& data) {
    files_[path] = data;
    unflushed_.erase(path);
    createDirs(parentDir(path));
}

std::string MemFileSystem::get(const std::string& path) const {
    auto it = files_.find(path);
    return it == files_.end() ? std::string() : it->second;
}

}  // namespace ss::persist
