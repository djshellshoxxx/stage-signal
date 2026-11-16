// Real desktop implementations of the small adapters (engine-adapters-desktop): durable filesystem, perf counters,
// ring log sink, safe ZIP, OS-keychain secret store, display provider, file dialogs.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include "Engine/IDisplayProvider.h"
#include "Engine/IDurableFileSystem.h"
#include "Engine/IFileDialogs.h"
#include "Engine/ILogSink.h"
#include "Engine/IPerfCounters.h"
#include "Engine/ISecretStoreAdapter.h"
#include "Engine/IZipArchive.h"
#include "ss/base/time.hpp"
#include "ss/persist/fs.hpp"

namespace ss::app {

// ---- durable filesystem: writeFile(flush=true) is followed by a real fsync (F_FULLFSYNC on macOS) ----------------------
class FsyncDurableFileSystem final : public IDurableFileSystem {
public:
    ss::Result<std::string> readFile(const std::string& p, std::uint64_t m) override { return fs_.readFile(p, m); }
    ss::Status writeFile(const std::string& p, const std::string& d, bool flush) override;
    ss::Status rename(const std::string& a, const std::string& b) override { return fs_.rename(a, b); }
    ss::Status remove(const std::string& p) override { return fs_.remove(p); }
    bool exists(const std::string& p) override { return fs_.exists(p); }
    ss::Result<std::vector<std::string>> listDir(const std::string& d) override { return fs_.listDir(d); }
    ss::Status createDirs(const std::string& d) override { return fs_.createDirs(d); }
    ss::Status syncFile(const std::string& path) override;
    ss::Status syncDirectory(const std::string& dir) override;
    ss::Result<std::uint64_t> freeBytes(const std::string& dir) override;
    bool folderWritable(const std::string& dir) override;
private:
    ss::persist::StdFileSystem fs_;
};

// ---- process CPU / working set (Linux /proc, macOS getrusage+mach, Windows GetProcessTimes+PSAPI) ----------------------
class ProcessPerfCounters final : public IPerfCounters {
public:
    explicit ProcessPerfCounters(std::shared_ptr<ss::IClock> clock) : clock_(std::move(clock)) {}
    PerfSample sample() override;
private:
    std::shared_ptr<ss::IClock> clock_;
    ss::Ns lastWall_ = 0;
    double lastCpuSec_ = -1.0;
};

// ---- bounded, rotated, sanitised file log. write() takes a short mutex; a writer thread owns the file. -----------------
class RingLogSink final : public ILogSink {
public:
    // dir empty -> <user app data>/StageSignal/logs. maxBytes/maxFiles default to spec: 5 MiB x 5.
    explicit RingLogSink(std::string dir = {}, std::uint64_t maxBytes = 5u << 20, int maxFiles = 5, std::size_t ringLines = 2048);
    ~RingLogSink() override;
    void write(LogLevel level, const std::string& sanitisedText) override;
    std::uint64_t dropped() const override { return dropped_.load(); }
    void flush();                                       // blocks until queued lines are on disk (tests, shutdown)
    std::string currentFileForTest() const;
private:
    void run();
    void appendLocked(const std::string& line);
    std::string dir_;
    std::uint64_t maxBytes_; int maxFiles_; std::size_t cap_;
    mutable std::mutex m_;
    std::condition_variable cv_, idleCv_;
    std::deque<std::string> q_;
    bool busy_ = false, stop_ = false;
    std::atomic<std::uint64_t> dropped_{0};
    std::thread th_;
    std::string curDay_;
    std::uint64_t curSize_ = 0;
};

// ---- safe ZIP: own central-directory reader (sizes, flags, symlinks) + juce::ZipFile for inflation -------------------
class SafeZipArchive final : public IZipArchive {
public:
    ss::Result<std::vector<ss::security::ArchiveEntryInfo>> list(const std::string& zipPath) override;
    ss::Result<std::string> readMember(const std::string& zipPath, std::size_t index, std::uint64_t maxBytes) override;
    ss::Status write(const std::string& zipPath, const std::vector<ZipMember>& members) override;
};

// ---- OS keychain. Never falls back to plaintext or a file; reports Unavailable instead. -------------------------------
class OsSecretStore final : public ISecretStoreAdapter {
public:
    OsSecretStore();
    ~OsSecretStore() override;
    ss::Status start() override;
    void stop() override;
    AdapterHealth health() const override;
    const char* backendName() const override;
    bool persistent() const override { return true; }
    ss::Status put(const ss::security::SecretRef& r, const std::string& v) override;
    ss::Result<std::string> get(const ss::security::SecretRef& r) const override;
    ss::Status remove(const ss::security::SecretRef& r) override;
    bool exists(const ss::security::SecretRef& r) const override;
    std::vector<ss::security::SecretRef> list() const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- displays: juce::Desktop::Displays. JUCE has no EDID/UUID, so identity is a hash of mode+scale (documented). -------
class JuceDisplayProvider final : public IDisplayProvider {
public:
    JuceDisplayProvider();
    ~JuceDisplayProvider() override;
    ss::Status start() override;
    void stop() override;
    AdapterHealth health() const override;
    ss::router::Topology topology() override;
    void setChangeCallback(std::function<void()> cb) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- native async file dialogs (juce::FileChooser::launchAsync). Message thread only. ----------------------------------
class JuceFileDialogs final : public IFileDialogs {
public:
    void launch(DialogKind kind, const std::string& startDir, std::function<void(const std::string&)> done) override;
};

}  // namespace ss::app
