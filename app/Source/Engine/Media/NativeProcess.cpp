#include "Engine/Media/NativeProcess.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <juce_core/juce_core.h>
#include "ss/record/profile.hpp"

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#else
 #include <errno.h>
 #include <fcntl.h>
 #include <signal.h>
 #include <spawn.h>
 #include <sys/types.h>
 #include <sys/wait.h>
 #include <unistd.h>
 extern char** environ;
#endif

namespace ss::app {
namespace {

constexpr std::size_t kMinQueueBytes = 4u << 20;
constexpr std::size_t kMaxReadPerCall = 256u << 10;

bool wantsAux(const std::vector<std::string>& argv) {
    return std::find(argv.begin(), argv.end(), "pipe:3") != argv.end();
}

std::vector<std::string> scrubbedEnv() {
    std::vector<std::string> out;
    for (const char* k : {"PATH", "HOME", "USERPROFILE", "SystemRoot", "TEMP", "TMP", "TMPDIR", "LANG", "LC_ALL", "LC_CTYPE"})
        if (const char* v = std::getenv(k)) out.push_back(std::string(k) + "=" + v);
    return out;
}

struct Chan {
#if JUCE_WINDOWS
    void* h = nullptr;
#else
    int fd = -1;
#endif
    std::string q;
    std::size_t head = 0;
    std::size_t cap = kMinQueueBytes;
    bool closeWhenDrained = false;
    std::uint64_t dropped = 0;
    bool open() const {
#if JUCE_WINDOWS
        return h != nullptr;
#else
        return fd >= 0;
#endif
    }
};

}  // namespace

class NativeSupervisor;

class NativeProcess final : public ss::broadcast::IProcess {
public:
    explicit NativeProcess(NativeSupervisor& sup) : sup_(sup) {}
    ~NativeProcess() override;

    ss::Status start(const std::vector<std::string>& argv) override;
    bool running() override;
    int exitCode() const override { return code_; }
    std::string readProgress() override { return drain(out_); }
    std::string readStderr() override { return drain(err_); }
    void requestStop() override;
    void kill() override;

    bool write(Chan& c, const std::uint8_t* d, std::size_t n);
    void pump();
    Chan in, aux;

private:
    struct Rd {
#if JUCE_WINDOWS
        void* h = nullptr;
#else
        int fd = -1;
#endif
    };
    std::string drain(Rd& r);
    void closeChan(Chan& c);
    void closeRd(Rd& r);
    void reaped();

    NativeSupervisor& sup_;
    Rd out_, err_;
    bool started_ = false, alive_ = false, killed_ = false, registered_ = false;
    int code_ = 0;
#if JUCE_WINDOWS
    void* proc_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
};

class NativeSupervisor final : public IProcessSupervisor {
public:
    ss::Status start() override {
#if !JUCE_WINDOWS
        ::signal(SIGPIPE, SIG_IGN);      // a closed child pipe must be an error code, not a process kill
#endif
        return ss::okStatus();
    }
    void stop() override {
        std::set<NativeProcess*> copy;
        { std::lock_guard<std::mutex> l(m_); copy = live_; }
        for (auto* p : copy) p->kill();
    }
    AdapterHealth health() const override { return {AdapterState::Running, {}}; }
    std::unique_ptr<ss::broadcast::IProcess> create() override { return std::make_unique<NativeProcess>(*this); }
    std::size_t liveChildren() const override { std::lock_guard<std::mutex> l(m_); return live_.size(); }

    bool writeStdin(ss::broadcast::IProcess& p, const std::uint8_t* d, std::size_t n) override {
        auto* np = dynamic_cast<NativeProcess*>(&p);
        return np && np->write(np->in, d, n);
    }
    bool writeAux(ss::broadcast::IProcess& p, const std::uint8_t* d, std::size_t n) override {
        auto* np = dynamic_cast<NativeProcess*>(&p);
        return np && np->write(np->aux, d, n);
    }
    bool auxSupported() const override {
#if JUCE_WINDOWS
        return false;
#else
        return true;
#endif
    }
    void pumpWrites(ss::broadcast::IProcess& p) override { if (auto* np = dynamic_cast<NativeProcess*>(&p)) np->pump(); }

    FfmpegProbe probeFfmpeg(const std::string& userPath) override;

    void add(NativeProcess* p) { std::lock_guard<std::mutex> l(m_); live_.insert(p); }
    void remove(NativeProcess* p) { std::lock_guard<std::mutex> l(m_); live_.erase(p); }

private:
    mutable std::mutex m_;
    std::set<NativeProcess*> live_;
};

// ------------------------------------------------------------------------------------------------ POSIX
#if !JUCE_WINDOWS
namespace {
int moveHigh(int fd) {                    // keep pipe ends out of 0..9 so dup2 onto 0..3 cannot clobber them
    int n = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
    ::close(fd);
    return n;
}
bool makePipe(int& r, int& w) {
    int f[2];
    if (::pipe(f) != 0) return false;
    r = moveHigh(f[0]); w = moveHigh(f[1]);
    if (r < 0 || w < 0) { if (r >= 0) ::close(r); if (w >= 0) ::close(w); return false; }
    return true;
}
void nonBlock(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
}  // namespace

ss::Status NativeProcess::start(const std::vector<std::string>& argv) {
    if (started_) return ss::makeError(ss::ErrorCategory::InternalFault, "process already started");
    if (argv.empty() || argv[0].empty()) return ss::makeError(ss::ErrorCategory::InvalidArgument, "no program to start");
    const bool useAux = wantsAux(argv);
    int inR = -1, inW = -1, outR = -1, outW = -1, errR = -1, errW = -1, auxR = -1, auxW = -1;
    auto closeAll = [&] { for (int* f : {&inR, &inW, &outR, &outW, &errR, &errW, &auxR, &auxW}) if (*f >= 0) { ::close(*f); *f = -1; } };
    if (!makePipe(inR, inW) || !makePipe(outR, outW) || !makePipe(errR, errW) || (useAux && !makePipe(auxR, auxW))) {
        closeAll();
        return ss::makeError(ss::ErrorCategory::ResourceExhausted, "cannot create pipes for the child process");
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inR, 0);
    posix_spawn_file_actions_adddup2(&fa, outW, 1);
    posix_spawn_file_actions_adddup2(&fa, errW, 2);
    if (useAux) posix_spawn_file_actions_adddup2(&fa, auxR, 3);
    std::vector<std::string> envStr = scrubbedEnv();
    std::vector<char*> envp, args;
    for (auto& e : envStr) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    const bool hasSlash = argv[0].find('/') != std::string::npos;
    const int rc = hasSlash ? ::posix_spawn(&pid, argv[0].c_str(), &fa, nullptr, args.data(), envp.data())
                            : ::posix_spawnp(&pid, argv[0].c_str(), &fa, nullptr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    ::close(inR); ::close(outW); ::close(errW);
    if (useAux) ::close(auxR);
    if (rc != 0) {
        for (int f : {inW, outR, errR, auxW}) if (f >= 0) ::close(f);
        return ss::makeError(ss::ErrorCategory::TransportFailure, "cannot start the program", ss::Recoverability::OperatorAction);
    }
    pid_ = pid; started_ = alive_ = true;
    in.fd = inW; nonBlock(inW);
    if (useAux) { aux.fd = auxW; nonBlock(auxW); }
    out_.fd = outR; nonBlock(outR);
    err_.fd = errR; nonBlock(errR);
#ifdef F_SETPIPE_SZ
    ::fcntl(inW, F_SETPIPE_SZ, 1 << 20);
    if (useAux) ::fcntl(auxW, F_SETPIPE_SZ, 1 << 20);
#endif
    sup_.add(this); registered_ = true;
    return ss::okStatus();
}

void NativeProcess::reaped() {
    alive_ = false;
    if (registered_) { sup_.remove(this); registered_ = false; }
    closeChan(in); closeChan(aux);
}

bool NativeProcess::running() {
    if (!alive_) return false;
    int st = 0;
    const pid_t r = ::waitpid(pid_, &st, WNOHANG);
    if (r == 0) return true;
    code_ = (r == pid_) ? (WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0)) : -1;
    reaped();
    return false;
}

void NativeProcess::kill() {
    if (alive_ && !killed_) { ::kill(pid_, SIGKILL); killed_ = true; }
}

NativeProcess::~NativeProcess() {
    if (alive_) {
        ::kill(pid_, SIGKILL);
        int st = 0;
        ::waitpid(pid_, &st, 0);
        reaped();
    }
    closeChan(in); closeChan(aux); closeRd(out_); closeRd(err_);
}

void NativeProcess::closeChan(Chan& c) { if (c.fd >= 0) { ::close(c.fd); c.fd = -1; } c.q.clear(); c.head = 0; }
void NativeProcess::closeRd(Rd& r) { if (r.fd >= 0) { ::close(r.fd); r.fd = -1; } }

std::string NativeProcess::drain(Rd& r) {
    std::string s;
    if (r.fd < 0) return s;
    char buf[8192];
    while (s.size() < kMaxReadPerCall) {
        const ssize_t n = ::read(r.fd, buf, sizeof buf);
        if (n > 0) { s.append(buf, static_cast<std::size_t>(n)); continue; }
        if (n == 0) { closeRd(r); }
        break;
    }
    return s;
}

void NativeProcess::pump() {
    for (Chan* c : {&in, &aux}) {
        while (c->fd >= 0 && c->head < c->q.size()) {
            const ssize_t n = ::write(c->fd, c->q.data() + c->head, c->q.size() - c->head);
            if (n > 0) { c->head += static_cast<std::size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            closeChan(*c);          // EPIPE or hard error: the child is gone
            break;
        }
        if (c->head > 0 && c->head == c->q.size()) { c->q.clear(); c->head = 0; }
        else if (c->head > (1u << 20)) { c->q.erase(0, c->head); c->head = 0; }
        if (c->fd >= 0 && c->closeWhenDrained && c->q.empty()) closeChan(*c);
    }
}

bool NativeProcess::write(Chan& c, const std::uint8_t* d, std::size_t n) {
    if (c.fd < 0 || c.closeWhenDrained) return false;
    c.cap = std::max(c.cap, n * 4);
    if (c.q.size() - c.head + n > c.cap) { ++c.dropped; return false; }
    c.q.append(reinterpret_cast<const char*>(d), n);
    pump();
    return true;
}

void NativeProcess::requestStop() {
    for (Chan* c : {&in, &aux}) c->closeWhenDrained = true;
    pump();
}
#else
// ------------------------------------------------------------------------------------------------ Windows
namespace {
std::wstring quoteArg(const std::string& a) {
    const std::wstring w = juce::String::fromUTF8(a.c_str()).toWideCharPointer();
    if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos) return w;
    std::wstring r = L"\"";
    std::size_t bs = 0;
    for (wchar_t c : w) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r.push_back(L'"'); bs = 0; continue; }
        r.append(bs, L'\\'); bs = 0; r.push_back(c);
    }
    r.append(bs * 2, L'\\'); r.push_back(L'"');
    return r;
}
}  // namespace

ss::Status NativeProcess::start(const std::vector<std::string>& argv) {
    if (started_) return ss::makeError(ss::ErrorCategory::InternalFault, "process already started");
    if (argv.empty() || argv[0].empty()) return ss::makeError(ss::ErrorCategory::InvalidArgument, "no program to start");
    if (wantsAux(argv)) return ss::makeError(ss::ErrorCategory::UnsupportedCapability, "a second input pipe is not available on Windows in this build");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 1 << 20) || !CreatePipe(&outR, &outW, &sa, 1 << 16) || !CreatePipe(&errR, &errW, &sa, 1 << 16))
        return ss::makeError(ss::ErrorCategory::ResourceExhausted, "cannot create pipes for the child process");
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    DWORD nowait = PIPE_NOWAIT;
    SetNamedPipeHandleState(inW, &nowait, nullptr, nullptr);
    std::wstring cmd;
    for (std::size_t i = 0; i < argv.size(); ++i) { if (i) cmd += L' '; cmd += quoteArg(argv[i]); }
    std::wstring env;
    for (auto& e : scrubbedEnv()) { env += juce::String::fromUTF8(e.c_str()).toWideCharPointer(); env.push_back(L'\0'); }
    env.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(), nullptr, &si, &pi);
    CloseHandle(inR); CloseHandle(outW); CloseHandle(errW);
    if (!ok) { CloseHandle(inW); CloseHandle(outR); CloseHandle(errR); return ss::makeError(ss::ErrorCategory::TransportFailure, "cannot start the program"); }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess; started_ = alive_ = true;
    in.h = inW; out_.h = outR; err_.h = errR;
    sup_.add(this); registered_ = true;
    return ss::okStatus();
}

void NativeProcess::reaped() {
    alive_ = false;
    if (registered_) { sup_.remove(this); registered_ = false; }
    closeChan(in);
}
bool NativeProcess::running() {
    if (!alive_) return false;
    DWORD c = 0;
    if (WaitForSingleObject(static_cast<HANDLE>(proc_), 0) == WAIT_TIMEOUT) return true;
    GetExitCodeProcess(static_cast<HANDLE>(proc_), &c);
    code_ = static_cast<int>(c);
    reaped();
    return false;
}
void NativeProcess::kill() { if (alive_ && !killed_) { TerminateProcess(static_cast<HANDLE>(proc_), 137); killed_ = true; } }
NativeProcess::~NativeProcess() {
    if (alive_) { TerminateProcess(static_cast<HANDLE>(proc_), 137); WaitForSingleObject(static_cast<HANDLE>(proc_), 2000); reaped(); }
    closeChan(in); closeRd(out_); closeRd(err_);
    if (proc_) CloseHandle(static_cast<HANDLE>(proc_));
}
void NativeProcess::closeChan(Chan& c) { if (c.h) { CloseHandle(static_cast<HANDLE>(c.h)); c.h = nullptr; } c.q.clear(); c.head = 0; }
void NativeProcess::closeRd(Rd& r) { if (r.h) { CloseHandle(static_cast<HANDLE>(r.h)); r.h = nullptr; } }
std::string NativeProcess::drain(Rd& r) {
    std::string s;
    if (!r.h) return s;
    char buf[8192];
    while (s.size() < kMaxReadPerCall) {
        DWORD avail = 0;
        if (!PeekNamedPipe(static_cast<HANDLE>(r.h), nullptr, 0, nullptr, &avail, nullptr)) { closeRd(r); break; }
        if (avail == 0) break;
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(r.h), buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || got == 0) break;
        s.append(buf, got);
    }
    return s;
}
void NativeProcess::pump() {
    Chan* c = &in;
    while (c->h && c->head < c->q.size()) {
        DWORD n = 0;
        if (!WriteFile(static_cast<HANDLE>(c->h), c->q.data() + c->head, static_cast<DWORD>(c->q.size() - c->head), &n, nullptr)) { closeChan(*c); break; }
        if (n == 0) break;
        c->head += n;
    }
    if (c->head > 0 && c->head == c->q.size()) { c->q.clear(); c->head = 0; }
    if (c->h && c->closeWhenDrained && c->q.empty()) closeChan(*c);
}
bool NativeProcess::write(Chan& c, const std::uint8_t* d, std::size_t n) {
    if (!c.h || c.closeWhenDrained) return false;
    c.cap = (std::max)(c.cap, n * 4);
    if (c.q.size() - c.head + n > c.cap) { ++c.dropped; return false; }
    c.q.append(reinterpret_cast<const char*>(d), n);
    pump();
    return true;
}
void NativeProcess::requestStop() { in.closeWhenDrained = true; pump(); }
#endif

// ------------------------------------------------------------------------------------------------ helpers
CapturedRun runCaptured(IProcessSupervisor& sup, const std::vector<std::string>& argv, int timeoutMs) {
    CapturedRun r;
    auto p = sup.create();
    if (!p->start(argv).ok()) return r;
    r.started = true;
    p->requestStop();                                     // nothing to send: EOF on stdin
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        r.out += p->readProgress();
        r.err += p->readStderr();
        if (!p->running()) { r.exited = true; r.code = p->exitCode(); break; }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeoutMs)) { p->kill(); r.timedOut = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    r.out += p->readProgress();
    r.err += p->readStderr();
    return r;
}

namespace {
std::string sanitizeLine(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\n' || c == '\r') break; if (static_cast<unsigned char>(c) >= 0x20) o.push_back(c); }
    if (o.size() > 120) o.resize(120);
    return o;
}
bool isExecutableFile(const juce::File& f) {
    if (!f.existsAsFile()) return false;
#if JUCE_WINDOWS
    return true;
#else
    return ::access(f.getFullPathName().toRawUTF8(), X_OK) == 0;
#endif
}
std::string discover(const std::string& userPath) {
#if JUCE_WINDOWS
    const char* exe = "ffmpeg.exe";
    const char sep = ';';
#else
    const char* exe = "ffmpeg";
    const char sep = ':';
#endif
    if (!userPath.empty()) {
        juce::File f(juce::String::fromUTF8(userPath.c_str()));
        return isExecutableFile(f) ? f.getFullPathName().toStdString() : std::string();
    }
    auto self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::File beside = self.getSiblingFile(exe);
    if (isExecutableFile(beside)) return beside.getFullPathName().toStdString();
    const char* path = std::getenv("PATH");
    if (!path) return {};
    std::string cur;
    for (const char* p = path;; ++p) {
        if (*p == sep || *p == 0) {
            if (!cur.empty()) {
                juce::File f = juce::File(juce::String::fromUTF8(cur.c_str())).getChildFile(exe);
                if (isExecutableFile(f)) return f.getFullPathName().toStdString();
            }
            cur.clear();
            if (*p == 0) break;
        } else cur.push_back(*p);
    }
    return {};
}
}  // namespace

FfmpegProbe NativeSupervisor::probeFfmpeg(const std::string& userPath) {
    FfmpegProbe pr;
    pr.path = discover(userPath);
    if (pr.path.empty()) {
        pr.reason = userPath.empty() ? "ffmpeg was not found next to the app or on PATH" : "The ffmpeg path in Setup does not point to a program";
        return pr;
    }
    auto v = runCaptured(*this, {pr.path, "-hide_banner", "-version"}, 3000);
    if (!v.exited || v.code != 0) { pr.reason = std::string("ffmpeg did not answer -version") + (v.started ? (v.timedOut ? " within 3 s" : " (exit code " + std::to_string(v.code) + ")") : " (could not be started)"); return pr; }
    pr.found = true;
    pr.versionLine = sanitizeLine(v.out);
    auto e = runCaptured(*this, {pr.path, "-hide_banner", "-encoders"}, 3000);
    pr.encodersText = e.out;
    pr.hasAac = e.out.find(" aac") != std::string::npos;
    pr.hasOpus = e.out.find("libopus") != std::string::npos;
    for (const auto& enc : ss::record::parseFfmpegEncoders(e.out)) {
        if (!enc.listed || !ss::record::isEligibleEncoder(enc.name)) continue;
        const bool h264 = enc.codec == ss::record::VideoCodec::H264;
        if (h264) pr.hasH264 = true;
        auto t = runCaptured(*this, {pr.path, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=c=black:s=256x256:d=0.1",
                                     "-frames:v", "1", "-c:v", enc.name, "-f", "null", "-"}, 3000);
        if (!(t.exited && t.code == 0)) continue;
        pr.verifiedEncoders.push_back(enc.name);
        (h264 ? pr.h264Verified : pr.mjpegVerified) = true;
    }
    if (!pr.h264Verified && !pr.mjpegVerified) pr.reason = "ffmpeg has no usable H.264 or MJPEG encoder";
    return pr;
}

std::shared_ptr<IProcessSupervisor> makeNativeProcessSupervisor() { return std::make_shared<NativeSupervisor>(); }

}  // namespace ss::app
