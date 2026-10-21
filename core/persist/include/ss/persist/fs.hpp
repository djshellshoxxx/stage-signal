// StageSignal persist: filesystem abstraction (spec section 17). Core code never touches the OS directly; the app may
// supply its own IFileSystem (for example with a real fsync). Paths are UTF-8 with '/' separators.
// Error detail never contains the path.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "ss/base/error.hpp"

namespace ss::persist {

class IFileSystem {
public:
    virtual ~IFileSystem() = default;
    // Fails with ResourceExhausted BEFORE reading when the file is larger than maxBytes.
    virtual Result<std::string> readFile(const std::string& path, std::uint64_t maxBytes) = 0;
    // Creates or truncates. flush=true asks for the data to be pushed to stable storage before returning.
    virtual Status writeFile(const std::string& path, const std::string& data, bool flush) = 0;
    // Atomically replaces `to` with `from` (same directory / filesystem).
    virtual Status rename(const std::string& from, const std::string& to) = 0;
    virtual Status remove(const std::string& path) = 0;          // idempotent
    virtual bool exists(const std::string& path) = 0;
    virtual Result<std::vector<std::string>> listDir(const std::string& dir) = 0;   // file names, sorted
    virtual Status createDirs(const std::string& dir) = 0;
};

std::string parentDir(const std::string& path);                 // "" when there is no '/'
std::string baseName(const std::string& path);
std::string joinPath(const std::string& dir, const std::string& name);

// std::filesystem implementation. flush=true flushes the stream; an fsync-capable adapter belongs to the app layer.
class StdFileSystem final : public IFileSystem {
public:
    Result<std::string> readFile(const std::string& path, std::uint64_t maxBytes) override;
    Status writeFile(const std::string& path, const std::string& data, bool flush) override;
    Status rename(const std::string& from, const std::string& to) override;
    Status remove(const std::string& path) override;
    bool exists(const std::string& path) override;
    Result<std::vector<std::string>> listDir(const std::string& dir) override;
    Status createDirs(const std::string& dir) override;
};

// In-memory filesystem with fault injection, used by every persistence test.
//  - Every mutating call (writeFile, rename, remove) increments opCount().
//  - A Fault at op N either simulates a crash (torn write for writeFile, nothing for rename/remove, then every call
//    fails until reboot()) or a DiskFull error (partial file left behind for writeFile).
//  - With loseUnflushedOnCrash, files last written with flush=false are emptied by a crash, which is how a missing
//    flush before rename is caught.
class MemFileSystem final : public IFileSystem {
public:
    enum class FaultKind : std::uint8_t { Crash, DiskFull };
    struct Fault { std::size_t atOp = 0; FaultKind kind = FaultKind::Crash; double tornFraction = 0.5; };

    Result<std::string> readFile(const std::string& path, std::uint64_t maxBytes) override;
    Status writeFile(const std::string& path, const std::string& data, bool flush) override;
    Status rename(const std::string& from, const std::string& to) override;
    Status remove(const std::string& path) override;
    bool exists(const std::string& path) override;
    Result<std::vector<std::string>> listDir(const std::string& dir) override;
    Status createDirs(const std::string& dir) override;

    // test helpers
    void put(const std::string& path, const std::string& data);   // direct, no fault accounting
    std::string get(const std::string& path) const;               // "" when missing
    void setFault(const Fault& f) { fault_ = f; hasFault_ = true; }
    void clearFault() { hasFault_ = false; }
    void reboot() { crashed_ = false; hasFault_ = false; }
    bool crashed() const { return crashed_; }
    std::size_t opCount() const { return ops_; }
    void resetOpCount() { ops_ = 0; }
    bool loseUnflushedOnCrash = true;
    std::size_t fileCount() const { return files_.size(); }

private:
    bool parentExists(const std::string& path) const;
    void crash();
    std::map<std::string, std::string> files_;
    std::set<std::string> dirs_;
    std::set<std::string> unflushed_;
    Fault fault_;
    bool hasFault_ = false, crashed_ = false;
    std::size_t ops_ = 0;
};

}  // namespace ss::persist
