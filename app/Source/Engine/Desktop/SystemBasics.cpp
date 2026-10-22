// Durable filesystem, process perf counters, ring log sink. OS specifics are guarded by JUCE_WINDOWS/JUCE_MAC/JUCE_LINUX.
#include <chrono>
#include <ctime>
#include <fstream>
#include <thread>
#include <algorithm>
#include <juce_core/juce_core.h>
#include "Engine/Desktop/DesktopCommon.h"
#include "Engine/Desktop/DesktopSystem.h"

#if JUCE_WINDOWS
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
 #pragma comment(lib, "psapi.lib")
#else
 #include <fcntl.h>
 #include <sys/resource.h>
 #include <sys/time.h>
 #include <unistd.h>
 #if JUCE_MAC
  #include <mach/mach.h>
 #endif
#endif

namespace ss::app {

namespace {
juce::File fileOf(const std::string& p) { return juce::File(juce::String::fromUTF8(p.c_str())); }
}

// ---------------------------------------------------------------------------------------------------------------------
ss::Status FsyncDurableFileSystem::writeFile(const std::string& p, const std::string& d, bool flush) {
    auto st = fs_.writeFile(p, d, flush);
    if (!st.ok() || !flush) return st;
    return syncFile(p);
}

ss::Status FsyncDurableFileSystem::syncFile(const std::string& path) {
#if JUCE_WINDOWS
    juce::FileOutputStream os(fileOf(path));          // opens without truncating; flush() = FlushFileBuffers
    if (os.failedToOpen()) return desktop::err(ss::ErrorCategory::TransportFailure, "cannot open file to sync");
    os.flush();
    return ss::okStatus();
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return desktop::err(ss::ErrorCategory::TransportFailure, "cannot open file to sync");
    int rc = -1;
  #if JUCE_MAC
    rc = ::fcntl(fd, F_FULLFSYNC);
    if (rc != 0) rc = ::fsync(fd);
  #else
    rc = ::fsync(fd);
  #endif
    ::close(fd);
    return rc == 0 ? ss::okStatus() : ss::Status(desktop::err(ss::ErrorCategory::TransportFailure, "fsync failed"));
#endif
}

ss::Status FsyncDurableFileSystem::syncDirectory(const std::string& dir) {
#if JUCE_WINDOWS
    // NTFS journals metadata; a directory handle flush needs backup semantics and is not required after MoveFileEx.
    (void) dir;
    return ss::okStatus();
#else
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) return desktop::err(ss::ErrorCategory::TransportFailure, "cannot open directory to sync");
    const int rc = ::fsync(fd);
    ::close(fd);
    return rc == 0 ? ss::okStatus() : ss::Status(desktop::err(ss::ErrorCategory::TransportFailure, "directory fsync failed"));
#endif
}

ss::Result<std::uint64_t> FsyncDurableFileSystem::freeBytes(const std::string& dir) {
    const auto n = fileOf(dir).getBytesFreeOnVolume();
    if (n < 0) return desktop::err(ss::ErrorCategory::TransportFailure, "free space unavailable");
    return static_cast<std::uint64_t>(n);
}

bool FsyncDurableFileSystem::folderWritable(const std::string& dir) {
    const juce::File d = fileOf(dir);
    if (!d.isDirectory()) return false;
    const juce::File t = d.getNonexistentChildFile(".ss-probe", ".tmp", false);
    const bool ok = t.create().wasOk();
    if (ok) t.deleteFile();
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
PerfSample ProcessPerfCounters::sample() {
    PerfSample s;
    s.logicalCores = std::max(1u, std::thread::hardware_concurrency());
    double cpuSec = -1.0;
#if JUCE_WINDOWS
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        auto v = [](const FILETIME& f) { return static_cast<double>((static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
        cpuSec = v(k) + v(u);
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) s.workingSetBytes = pmc.WorkingSetSize;
#else
    rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) == 0)
        cpuSec = static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
  #if JUCE_MAC
    mach_task_basic_info info{};
    mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &cnt) == KERN_SUCCESS)
        s.workingSetBytes = info.resident_size;
  #else
    std::ifstream f("/proc/self/statm");
    std::uint64_t total = 0, res = 0;
    if (f >> total >> res) s.workingSetBytes = res * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
  #endif
#endif
    const ss::Ns now = clock_->nowMonotonic();
    if (cpuSec >= 0.0 && lastCpuSec_ >= 0.0 && now > lastWall_) {
        const double wall = static_cast<double>(now - lastWall_) * 1e-9;
        s.processCpuPct = std::clamp(100.0 * (cpuSec - lastCpuSec_) / (wall * s.logicalCores), 0.0, 100.0);
        s.valid = true;
    }
    if (cpuSec >= 0.0) { lastCpuSec_ = cpuSec; lastWall_ = now; }
    return s;                         // first call has no interval: valid=false
}

// ---------------------------------------------------------------------------------------------------------------------
namespace {
std::string utcStamp(bool dayOnly) {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if JUCE_WINDOWS
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[40];
    std::strftime(b, sizeof(b), dayOnly ? "%Y-%m-%d" : "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}
const char* levelName(LogLevel l) {
    switch (l) { case LogLevel::Error: return "ERROR"; case LogLevel::Warn: return "WARN"; case LogLevel::Info: return "INFO"; default: return "DEBUG"; }
}
}  // namespace

RingLogSink::RingLogSink(std::string dir, std::uint64_t maxBytes, int maxFiles, std::size_t ringLines)
    : dir_(std::move(dir)), maxBytes_(maxBytes), maxFiles_(std::max(1, maxFiles)), cap_(ringLines) {
    if (dir_.empty())
        dir_ = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("StageSignal").getChildFile("logs")
                   .getFullPathName().toStdString();
    th_ = std::thread([this] { run(); });
}

RingLogSink::~RingLogSink() {
    { std::lock_guard<std::mutex> g(m_); stop_ = true; }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void RingLogSink::write(LogLevel level, const std::string& text) {
    std::string line = utcStamp(false) + " " + levelName(level) + " " + ss::security::sanitizeForLog(text, 512) + "\n";
    {
        std::lock_guard<std::mutex> g(m_);
        if (q_.size() >= cap_) { q_.pop_front(); dropped_.fetch_add(1); }       // oldest line is dropped and counted
        q_.push_back(std::move(line));
    }
    cv_.notify_one();
}

void RingLogSink::flush() {
    std::unique_lock<std::mutex> g(m_);
    idleCv_.wait_for(g, std::chrono::seconds(5), [&] { return q_.empty() && !busy_; });
}

std::string RingLogSink::currentFileForTest() const {
    std::lock_guard<std::mutex> g(m_);
    return (juce::File(juce::String::fromUTF8(dir_.c_str())).getChildFile("stagesignal-" + juce::String(curDay_) + ".log")).getFullPathName().toStdString();
}

void RingLogSink::run() {
    for (;;) {
        std::deque<std::string> batch;
        {
            std::unique_lock<std::mutex> g(m_);
            cv_.wait(g, [&] { return stop_ || !q_.empty(); });
            if (q_.empty() && stop_) return;
            batch.swap(q_);
            busy_ = true;
        }
        for (const auto& l : batch) appendLocked(l);
        {
            std::lock_guard<std::mutex> g(m_);
            busy_ = false;
        }
        idleCv_.notify_all();
    }
}

void RingLogSink::appendLocked(const std::string& line) {          // writer thread only
    const juce::File dir = fileOf(dir_);
    if (!dir.createDirectory().wasOk()) { dropped_.fetch_add(1); return; }
    const std::string day = utcStamp(true);
    const juce::File base = dir.getChildFile("stagesignal-" + juce::String(day) + ".log");
    if (day != curDay_) {
        { std::lock_guard<std::mutex> g(m_); curDay_ = day; }
        curSize_ = base.existsAsFile() ? static_cast<std::uint64_t>(base.getSize()) : 0;
        // Bound the number of files: keep the newest maxFiles_ (by modification time).
        auto files = dir.findChildFiles(juce::File::findFiles, false, "stagesignal-*");
        files.sort();
        while (files.size() > maxFiles_) { if (files[0] != base) files[0].deleteFile(); files.remove(0); }
    }
    if (curSize_ + line.size() > maxBytes_) {                       // rotate base -> .1 -> .2 ... ; the oldest is deleted
        dir.getChildFile(base.getFileName() + "." + juce::String(maxFiles_ - 1)).deleteFile();
        for (int i = maxFiles_ - 1; i >= 2; --i) {
            const juce::File from = dir.getChildFile(base.getFileName() + "." + juce::String(i - 1));
            if (from.existsAsFile()) from.moveFileTo(dir.getChildFile(base.getFileName() + "." + juce::String(i)));
        }
        if (maxFiles_ >= 2) base.moveFileTo(dir.getChildFile(base.getFileName() + ".1")); else base.deleteFile();
        curSize_ = 0;
    }
    if (base.appendText(juce::String::fromUTF8(line.c_str(), static_cast<int>(line.size())), false, false, "\n")) curSize_ += line.size();
    else dropped_.fetch_add(1);
}

}  // namespace ss::app
