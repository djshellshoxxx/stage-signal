// Contract: persist's IFileSystem plus durability and space probes (app-adapters 9, APP-AD-10).
// Owner: engine-adapters-desktop.
#pragma once
#include <cstdint>
#include "Engine/AdapterCommon.h"
#include "ss/persist/fs.hpp"

namespace ss::app {

class IDurableFileSystem : public ss::persist::IFileSystem {
public:
    virtual ss::Status syncFile(const std::string& path) = 0;        // fsync the file
    virtual ss::Status syncDirectory(const std::string& dir) = 0;    // fsync after atomic rename
    virtual ss::Result<std::uint64_t> freeBytes(const std::string& dir) = 0;
    virtual bool folderWritable(const std::string& dir) = 0;
};

// Std filesystem WITHOUT real durability: syncFile/syncDirectory succeed without forcing data to disk.
// Fake for tests and headless smoke only; engine-adapters-desktop replaces it with an fsync implementation.
class StdDurableFileSystem final : public IDurableFileSystem {
public:
    ss::Result<std::string> readFile(const std::string& p, std::uint64_t m) override { return fs_.readFile(p, m); }
    ss::Status writeFile(const std::string& p, const std::string& d, bool f) override { return fs_.writeFile(p, d, f); }
    ss::Status rename(const std::string& a, const std::string& b) override { return fs_.rename(a, b); }
    ss::Status remove(const std::string& p) override { return fs_.remove(p); }
    bool exists(const std::string& p) override { return fs_.exists(p); }
    ss::Result<std::vector<std::string>> listDir(const std::string& d) override { return fs_.listDir(d); }
    ss::Status createDirs(const std::string& d) override { return fs_.createDirs(d); }
    ss::Status syncFile(const std::string&) override { return ss::okStatus(); }
    ss::Status syncDirectory(const std::string&) override { return ss::okStatus(); }
    ss::Result<std::uint64_t> freeBytes(const std::string&) override { return std::uint64_t{1} << 40; }
    bool folderWritable(const std::string&) override { return true; }
private:
    ss::persist::StdFileSystem fs_;
};

}  // namespace ss::app
