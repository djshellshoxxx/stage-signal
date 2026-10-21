// Owner: engine-adapters-desktop. Headless H-tests for the real desktop adapters (app-adapters section 15).
// No hardware is required: every test must pass with no audio device, no MIDI port, no keychain and no display
// beyond what Xvfb gives. Tests run on the JUCE message thread (StageSignalApp::initialise).
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "App/LightingScheduler.h"
#include "App/MidiScheduler.h"
#include "Engine/DesktopAdapters.h"
#include "Engine/Desktop/DesktopSystem.h"
#include "Engine/Desktop/JuceAudioSource.h"
#include "Engine/Desktop/JuceMidiProvider.h"
#include "Engine/Desktop/TimestampMapperLinear.h"
#include "Engine/Desktop/UdpPacketSink.h"
#include "Engine/Fakes.h"
#include "ss/lighting/fixture.hpp"
#include "Tests/SelfTest.h"

namespace ss::app {

namespace {

SelfTestResult fail(const std::string& m) { return {false, m}; }
#define REQUIRE_T(cond, msg) do { if (!(cond)) return fail(msg); } while (0)

struct TempDir {
    juce::File dir;
    TempDir() : dir(juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("ss-adapters-test", "", false)) { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
    std::string path(const char* leaf) const { return dir.getChildFile(leaf).getFullPathName().toStdString(); }
};

// ---- minimal ZIP writer for hostile archives (stored entries, CRC left 0: only the structure matters) -----------------
struct RawEntry { std::string name, data; std::uint16_t flags = 0, madeBy = 20; std::uint32_t extAttr = 0; std::uint32_t declaredUncompressed = 0xFFFFFFFE; };
void p16(std::string& s, std::uint32_t v) { s += static_cast<char>(v & 0xFF); s += static_cast<char>((v >> 8) & 0xFF); }
void p32(std::string& s, std::uint32_t v) { p16(s, v & 0xFFFF); p16(s, v >> 16); }
std::string rawZip(const std::vector<RawEntry>& es) {
    std::string out, cd;
    for (const auto& e : es) {
        const std::uint32_t off = static_cast<std::uint32_t>(out.size());
        const std::uint32_t usz = e.declaredUncompressed == 0xFFFFFFFEu ? static_cast<std::uint32_t>(e.data.size()) : e.declaredUncompressed;
        p32(out, 0x04034b50); p16(out, 20); p16(out, e.flags); p16(out, 0); p16(out, 0); p16(out, 0x21); p32(out, 0);
        p32(out, static_cast<std::uint32_t>(e.data.size())); p32(out, usz); p16(out, static_cast<std::uint32_t>(e.name.size())); p16(out, 0);
        out += e.name; out += e.data;
        p32(cd, 0x02014b50); p16(cd, e.madeBy); p16(cd, 20); p16(cd, e.flags); p16(cd, 0); p16(cd, 0); p16(cd, 0x21); p32(cd, 0);
        p32(cd, static_cast<std::uint32_t>(e.data.size())); p32(cd, usz); p16(cd, static_cast<std::uint32_t>(e.name.size()));
        p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, e.extAttr); p32(cd, off); cd += e.name;
    }
    const std::uint32_t cdOff = static_cast<std::uint32_t>(out.size());
    out += cd;
    p32(out, 0x06054b50); p16(out, 0); p16(out, 0); p16(out, static_cast<std::uint32_t>(es.size())); p16(out, static_cast<std::uint32_t>(es.size()));
    p32(out, static_cast<std::uint32_t>(cd.size())); p32(out, cdOff); p16(out, 0);
    return out;
}
bool writeBin(const std::string& path, const std::string& d) { std::ofstream f(path, std::ios::binary); f.write(d.data(), static_cast<std::streamsize>(d.size())); return f.good(); }

// "Rejected" = list() or validation or readMember refuses, and nothing is ever returned for the hostile member.
bool zipRejected(SafeZipArchive& z, const std::string& path) {
    auto l = z.list(path);
    if (!l.ok()) return true;
    if (!ss::security::validateArchive(l.value()).ok()) return true;
    return !z.readMember(path, 0, 1 << 20).ok();
}

SelfTestResult zipSafe() {
    TempDir td; SafeZipArchive z;
    // 1 round trip + determinism
    std::vector<ZipMember> ms = {{"show.json", "{\"a\":1}"}, {"media/readme.txt", std::string(5000, 'x')}};
    const auto p1 = td.path("a.zip"), p2 = td.path("b.zip");
    REQUIRE_T(z.write(p1, ms).ok() && z.write(p2, ms).ok(), "zip write failed");
    juce::MemoryBlock b1, b2;
    REQUIRE_T(juce::File(p1).loadFileAsData(b1) && juce::File(p2).loadFileAsData(b2) && b1 == b2, "zip output not deterministic");
    auto l = z.list(p1);
    REQUIRE_T(l.ok() && l.value().size() == 2 && ss::security::validateArchive(l.value()).ok(), "zip list/validate failed");
    auto m = z.readMember(p1, 1, 1 << 20);
    REQUIRE_T(m.ok() && m.value() == ms[1].bytes, "zip member round trip failed");
    REQUIRE_T(!z.readMember(p1, 1, 100).ok(), "zip member size cap not enforced");
    REQUIRE_T(!z.readMember(p1, 9, 1 << 20).ok(), "zip index bound not enforced");
    REQUIRE_T(!juce::File(p1 + ".tmp").exists(), "temp file left behind");
    // 2 writer refuses unsafe names
    for (const char* bad : {"../evil.txt", "/abs.txt", "a\\b.txt", "C:/x.txt", "a/../b.txt", ".hidden"})
        REQUIRE_T(!z.write(td.path("bad.zip"), {{bad, "x"}}).ok(), std::string("writer accepted ") + bad);
    REQUIRE_T(!z.write(td.path("dup.zip"), {{"A.txt", "1"}, {"a.txt", "2"}}).ok(), "case-colliding names accepted");
    // 3 reader refuses hostile archives
    struct Case { const char* what; std::vector<RawEntry> es; };
    RawEntry sym{"link", "target"}; sym.madeBy = 3 << 8 | 20; sym.extAttr = 0120777u << 16;
    RawEntry enc{"secret.txt", "zz"}; enc.flags = 1;
    RawEntry bomb{"bomb.bin", "x"}; bomb.declaredUncompressed = 900u << 20;     // ratio far above 100:1
    RawEntry lie{"liar.txt", std::string(10, 'a')}; lie.declaredUncompressed = 3;
    std::vector<Case> cases = {
        {"dotdot", {{"../evil.txt", "x"}}}, {"abs", {{"/etc/passwd", "x"}}}, {"drive", {{"C:/win.txt", "x"}}},
        {"backslash", {{"a\\..\\b.txt", "x"}}}, {"nested-dotdot", {{"a/../../b.txt", "x"}}}, {"nul", {{std::string("a\0b.txt", 7), "x"}}},
        {"symlink", {sym}}, {"encrypted", {enc}}, {"bomb", {bomb}}, {"dup-case", {{"A.txt", "1"}, {"a.txt", "2"}}},
    };
    for (const auto& c : cases) {
        const auto p = td.path("h.zip");
        REQUIRE_T(writeBin(p, rawZip(c.es)), "test fixture write failed");
        REQUIRE_T(zipRejected(z, p), std::string("hostile zip accepted: ") + c.what);
    }
    // declared size smaller than real data: the inflate path must stop at the declared size, never over-read
    REQUIRE_T(writeBin(td.path("lie.zip"), rawZip({lie})), "fixture");
    REQUIRE_T(!z.readMember(td.path("lie.zip"), 0, 1 << 20).ok(), "size lie accepted");
    // garbage / truncated
    REQUIRE_T(writeBin(td.path("junk.zip"), "not a zip at all, definitely not"), "fixture");
    REQUIRE_T(!z.list(td.path("junk.zip")).ok(), "junk accepted");
    REQUIRE_T(!z.list(td.path("missing.zip")).ok(), "missing file accepted");
    return {true, "round trip, determinism, 10 hostile archives and 6 hostile names rejected"};
}

SelfTestResult udpLoopback() {
    UdpPacketSink s;
    ss::lighting::Datagram d;
    d.dst.ip.b = {{127, 0, 0, 1}}; d.bytes = {'A', 'r', 't', '-', 'N', 'e', 't', 0, 1, 2, 3};
    REQUIRE_T(s.start().ok() && s.start().ok(), "start not idempotent");
    REQUIRE_T(!s.connected() && !s.send(d).ok(), "sent before an interface was chosen");
    REQUIRE_T(s.health().state == AdapterState::Degraded, "unbound sink should be Degraded");
    REQUIRE_T(!s.bindInterface("999.1.1.1").ok(), "bad address accepted");
    juce::DatagramSocket rx(false);
    REQUIRE_T(rx.bindToPort(0, "127.0.0.1"), "cannot bind receiver");
    d.dst.port = static_cast<std::uint16_t>(rx.getBoundPort());
    REQUIRE_T(s.bindInterface("127.0.0.1").ok() && s.connected(), "bind to loopback failed");
    REQUIRE_T(s.health().state == AdapterState::Running, "bound sink should be Running");
    REQUIRE_T(s.send(d).ok(), "loopback send failed");
    REQUIRE_T(rx.waitUntilReady(true, 1000) == 1, "datagram not received");
    char buf[64] = {};
    const int n = rx.read(buf, sizeof(buf), false);
    REQUIRE_T(n == static_cast<int>(d.bytes.size()) && std::memcmp(buf, d.bytes.data(), d.bytes.size()) == 0, "payload differs");
    ss::lighting::Datagram big = d; big.bytes.assign(2000, 1);
    REQUIRE_T(!s.send(big).ok(), "oversize datagram accepted");
    REQUIRE_T(!s.listInterfaces().empty(), "no interfaces listed");
    s.stop(); s.stop();
    REQUIRE_T(!s.connected() && !s.send(d).ok(), "sent after stop");
    REQUIRE_T(s.health().state == AdapterState::Stopped, "stopped sink state");
    return {true, "1 packet byte-exact on loopback; nothing before bind or after stop"};
}

SelfTestResult durableFs() {
    TempDir td; FsyncDurableFileSystem fs;
    const auto a = td.path("doc.json"), t = td.path("doc.json.tmp");
    REQUIRE_T(fs.writeFile(a, "old", true).ok(), "write+fsync failed");
    REQUIRE_T(fs.writeFile(t, "new-content", true).ok() && fs.syncFile(t).ok(), "tmp write/fsync failed");
    REQUIRE_T(fs.rename(t, a).ok() && fs.syncDirectory(td.path("")).ok(), "atomic rename/dir sync failed");
    auto r = fs.readFile(a, 1024);
    REQUIRE_T(r.ok() && r.value() == "new-content" && !fs.exists(t), "rename did not replace atomically");
    REQUIRE_T(!fs.syncFile(td.path("nope")).ok(), "sync of a missing file succeeded");
    auto fb = fs.freeBytes(td.path(""));
    REQUIRE_T(fb.ok() && fb.value() > 0, "free space probe failed");
    REQUIRE_T(fs.folderWritable(td.path("")) && !fs.folderWritable(td.path("missing")), "writable probe wrong");
    REQUIRE_T(!fs.readFile(a, 3).ok(), "read cap not enforced");
    return {true, "write+fsync, atomic rename, dir sync, free space"};
}

SelfTestResult midiNoPorts() {
    auto clock = std::make_shared<ss::SystemClock>();
    JuceMidiProvider p(clock);
    REQUIRE_T(p.health().state == AdapterState::Stopped, "initial state");
    REQUIRE_T(p.start().ok() && p.start().ok(), "start not idempotent");
    REQUIRE_T(p.health().state == AdapterState::Running, "provider with zero ports must be healthy");
    (void) p.listPorts();                                                   // may be empty; must not throw
    struct Sink final : IMidiInputSink { void onMidiBytes(std::uint32_t, const std::uint8_t*, std::uint32_t, ss::Ns) override {} } sink;
    REQUIRE_T(!p.openInput("no-such-device-identity", &sink).ok(), "opened a device that does not exist");
    REQUIRE_T(!p.openOutput("no-such-device-identity").ok(), "opened an output that does not exist");
    p.closeInput(12345); p.closeOutput(nullptr);                           // unknown ids are no-ops
    p.stop(); p.stop();
    REQUIRE_T(p.health().state == AdapterState::Stopped, "stop state");
    // The scheduler driver survives an empty provider and counts nothing.
    JuceMidiProvider p2(clock); (void) p2.start();
    MidiScheduler sch(*clock, p2);
    sch.driver().wantInput("absent");
    for (int i = 0; i < 3; ++i) sch.tick();
    MidiView v; sch.fillView(v);
    REQUIRE_T(v.inputPorts == 0 && v.messagesSent == 0 && !v.outputEnabled, "midi scheduler view");
    return {true, "healthy with no ports; open of absent device refused; scheduler idle"};
}

struct CountingSink final : IAudioSink {
    std::atomic<int> blocks{0}, opened{0}, lost{0};
    float first[4] = {0, 0, 0, 0}; std::uint32_t ch = 0, frames = 0, rate = 0;
    void pushBlock(const float* il, std::uint32_t f, std::uint32_t c, std::uint32_t r, ss::Ns) override {
        if (blocks.fetch_add(1) == 0) for (std::uint32_t i = 0; i < 4 && i < f * c; ++i) first[i] = il[i];
        ch = c; frames = f; rate = r;
    }
    void streamOpened(const AudioStreamFormat&) override { ++opened; }
    void streamLost() override { ++lost; }
};

SelfTestResult audioNoDevice() {
    auto clock = std::make_shared<ss::SystemClock>();
    auto mapper = std::make_shared<LinearFitTimestampMapper>();
    JuceAudioSource a(clock, mapper);
    CountingSink sink;
    a.setSink(&sink);
    REQUIRE_T(a.health().state == AdapterState::Stopped, "initial state");
    const auto st = a.start();                       // headless: usually no device -> Unavailable, but never a crash
    const auto h = a.health();
    REQUIRE_T(st.ok() ? (h.state == AdapterState::Running || h.state == AdapterState::Degraded)
                      : (h.state == AdapterState::Unavailable && !h.lastError.empty()),
              "start result and health disagree");
    (void) a.start();                                // idempotent
    (void) a.listDevices();                          // must not crash without hardware
    REQUIRE_T(!a.selectDevice({"", "", 0, {}}).ok(), "empty device selection accepted");
    a.stop(); a.stop();
    REQUIRE_T(a.health().state == AdapterState::Stopped && !a.hasDevice(), "stop state");
    // Simulated backend callbacks: interleave, loss, return.
    JuceAudioSource b(clock, mapper);
    b.setSink(&sink);
    b.simulateStarted(48000.0, 256);
    std::vector<float> l(256), r(256);
    for (int i = 0; i < 256; ++i) { l[static_cast<std::size_t>(i)] = static_cast<float>(i); r[static_cast<std::size_t>(i)] = -static_cast<float>(i); }
    const float* chans[2] = {l.data(), r.data()};
    b.simulateBlock(chans, 2, 256);
    REQUIRE_T(sink.blocks == 1 && sink.ch == 2 && sink.frames == 256 && sink.rate == 48000, "block not delivered");
    REQUIRE_T(std::fabs(sink.first[0]) < 1e-6f && std::fabs(sink.first[1]) < 1e-6f && std::fabs(sink.first[2] - 1.f) < 1e-6f && std::fabs(sink.first[3] + 1.f) < 1e-6f && sink.opened == 1, "interleaving or open event wrong");
    b.simulateBlock(chans, 2, 1000);                 // oversize callback is split, never reallocates
    REQUIRE_T(sink.blocks > 2, "oversize callback not split");
    b.simulateStopped();
    REQUIRE_T(sink.lost == 1 && !b.hasDevice() && b.health().state == AdapterState::Degraded, "loss not reported");
    b.simulateStarted(48000.0, 256);
    REQUIRE_T(sink.opened == 2 && b.hasDevice() && b.health().state == AdapterState::Running, "return not reported");
    b.stop();
    return {true, std::string("no-device start ") + adapterStateName(h.state) + "; simulated loss/return ok"};
}

SelfTestResult mapper() {
    LinearFitTimestampMapper m;
    const std::uint32_t rate = 48000, block = 256;
    const double period = 1e9 * block / rate;
    auto run = [&](double jitterNs, int blocks, bool outlier) {
        m.reset();
        std::uint32_t seed = 12345;
        bool anyValidEarly = false, valid = false;
        for (int i = 0; i < blocks; ++i) {
            seed = seed * 1664525u + 1013904223u;
            double j = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 2.0 * jitterNs;
            if (outlier && i == blocks / 2) j += 50e6;
            const ss::Ns entry = 1'000'000'000LL + static_cast<ss::Ns>(i * period * 1.00002 + j);   // 20 ppm drift
            auto r = m.map(static_cast<std::uint64_t>(i) * block, rate, entry);
            if (i < 100 && r.valid) anyValidEarly = true;
            valid = r.valid;
        }
        return std::make_pair(anyValidEarly, valid);
    };
    auto good = run(200e3, 600, false);
    REQUIRE_T(!good.first, "valid before 1 s of data");
    REQUIRE_T(good.second && m.state() == MapperState::Converged, "did not converge with 0.2 ms jitter");
    auto out = run(200e3, 600, true);
    REQUIRE_T(out.second && m.state() == MapperState::Converged && m.outliersRejected() >= 1, "outlier not rejected");
    auto bad = run(20e6, 600, false);
    REQUIRE_T(!bad.second && m.state() == MapperState::Degraded, "20 ms jitter must be Degraded and invalid");
    m.reset();
    REQUIRE_T(m.state() == MapperState::Unconverged, "reset state");
    return {true, "converges <2 ms, rejects outlier, degraded at 20 ms jitter, never valid when unconverged"};
}

SelfTestResult secrets() {
    OsSecretStore s;
    REQUIRE_T(s.start().ok() && s.start().ok(), "start must be idempotent and never fatal");
    const auto h = s.health();
    const std::string ref = ss::security::makeSecretRef("selftest-probe");
    if (h.state != AdapterState::Running) {
        REQUIRE_T(h.state == AdapterState::Unavailable && !h.lastError.empty(), "no keychain must be reported as Unavailable");
        REQUIRE_T(!s.put(ref, "value-should-not-be-stored").ok(), "secret accepted without a keychain");
        REQUIRE_T(!s.get(ref).ok() && !s.exists(ref) && s.list().empty(), "secret readable without a keychain");
        return {true, "no keychain: refused and reported Unavailable, nothing stored"};
    }
    REQUIRE_T(s.put(ref, "s3cret-value").ok(), "keychain put failed");
    auto g = s.get(ref);
    const bool same = g.ok() && g.value() == "s3cret-value";
    (void) s.remove(ref);
    REQUIRE_T(same && !s.get(ref).ok(), "keychain round trip failed");
    return {true, std::string("keychain round trip via ") + s.backendName()};
}

SelfTestResult logRing() {
    TempDir td;
    {
        RingLogSink log(td.path("logs"), 2048, 3, 4096);
        for (int i = 0; i < 300; ++i) log.write(LogLevel::Info, "line " + std::to_string(i) + " " + std::string(80, 'z') + "\ttab\nnewline");
        log.flush();
        REQUIRE_T(log.dropped() == 0, "unexpected drops");
        int files = 0; juce::int64 maxSize = 0; juce::String all;
        for (const auto& f : juce::File(td.path("logs")).findChildFiles(juce::File::findFiles, false)) {
            ++files; maxSize = std::max(maxSize, f.getSize()); all += f.loadFileAsString();
        }
        REQUIRE_T(files >= 2 && files <= 3, "rotation did not bound the file count");
        REQUIRE_T(maxSize <= 2048, "file exceeded the rotation size");
        REQUIRE_T(all.contains("INFO line 299") && !all.contains("\ttab"), "content or control-char sanitising wrong");
    }
    RingLogSink tiny(td.path("logs2"), 1 << 20, 2, 2);          // 2-line ring: overflow is dropped and counted, never blocks
    for (int i = 0; i < 5000; ++i) tiny.write(LogLevel::Debug, "x");
    tiny.flush();
    return {true, "rotation bounded, sanitised; overflow counted=" + std::to_string(tiny.dropped())};
}

SelfTestResult perf() {
    auto clock = std::make_shared<ss::SystemClock>();
    ProcessPerfCounters p(clock);
    (void) p.sample();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    volatile double sink = 0; while (std::chrono::steady_clock::now() < until) sink = sink + std::sqrt(sink + 1.0);
    const auto s = p.sample();
    REQUIRE_T(s.valid && s.logicalCores >= 1, "second sample invalid");
    REQUIRE_T(s.workingSetBytes > (1u << 20), "working set not read");
    REQUIRE_T(s.processCpuPct > 0.0 && s.processCpuPct <= 100.0, "cpu out of range");
    return {true, "cpu " + std::to_string(static_cast<int>(s.processCpuPct)) + "% rss " + std::to_string(s.workingSetBytes >> 20) + " MiB"};
}

SelfTestResult displays() {
    JuceDisplayProvider d;
    REQUIRE_T(d.start().ok() && d.start().ok(), "display start");
    const auto t = d.topology();
    for (const auto& p : t) REQUIRE_T(!p.identity.empty() && !p.connector.empty() && p.width > 0, "malformed display record");
    REQUIRE_T(d.health().state == AdapterState::Degraded, "identity limitation must be reported");
    d.stop(); d.stop();
    return {true, std::to_string(t.size()) + " display(s), identity limitation reported"};
}

SelfTestResult lightingGate() {
    // LightingScheduler + real UdpPacketSink on loopback: no packet while disarmed, packets once armed, none after disarm.
    ss::SystemClock clock;
    UdpPacketSink udp; (void) udp.start();
    juce::DatagramSocket rx(false);
    REQUIRE_T(rx.bindToPort(6454, "127.0.0.1") || rx.bindToPort(0, "127.0.0.1"), "cannot bind receiver");
    if (rx.getBoundPort() != 6454) return {true, "skipped: Art-Net port 6454 busy on this host"};
    BlackoutLatch latch;
    ss::LatestMailbox<ss::ShowSnapshotLite> show;
    ss::LatestMailbox<ss::FeatureSnapshot> feat;
    LightingScheduler sch(clock, udp, latch, show, feat);
    REQUIRE_T(!sch.arm().ok(), "arm must be refused while no interface is chosen");
    REQUIRE_T(udp.bindInterface("127.0.0.1").ok(), "bind");
    sch.driver().post([](ss::lighting::LightingEngine& e) {
        ss::lighting::LightingConfig c; c.destination.b = {{127, 0, 0, 1}};
        ss::model::FixturePatch p; p.id = 1; p.profileId = ss::lighting::kProfileRgbStrobe; p.universe = 0; p.startAddress = 1;
        (void) e.configure(ss::lighting::builtinProfiles(), {p}, c);
    });
    auto pump = [&](int n) { for (int i = 0; i < n; ++i) { sch.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); } };
    pump(15);
    REQUIRE_T(udp.sentCount() == 0 && rx.waitUntilReady(true, 50) != 1, "packet left the process while disarmed");
    REQUIRE_T(sch.arm().ok(), "arm refused");
    pump(20);
    LightingView v; sch.fillView(v);
    REQUIRE_T(v.state == LightingUiState::Armed && v.engineArmed && udp.sentCount() > 0, "no packets while armed: " + v.note);
    char buf[600] = {};
    REQUIRE_T(rx.waitUntilReady(true, 200) == 1 && rx.read(buf, sizeof(buf), false) > 18 && std::memcmp(buf, "Art-Net", 8) == 0, "not an Art-Net packet");
    sch.disarm();
    pump(2);                                         // the engine's safe-frame sequence may send during the first tick
    const auto after = udp.sentCount();
    pump(15);
    REQUIRE_T(udp.sentCount() == after, "packets after disarm");
    // Transport loss while armed drops the gate and requires a new Arm.
    REQUIRE_T(sch.arm().ok(), "re-arm refused"); pump(5);
    udp.stop();
    pump(5);
    sch.fillView(v);
    REQUIRE_T(!sch.armed() && v.state == LightingUiState::Fault, "transport loss did not disarm");
    return {true, "0 packets disarmed, Art-Net packets armed, 0 after disarm, disarm on transport loss"};
}

SelfTestResult factorySet() {
    auto s = makeDesktopAdapterSet();
    REQUIRE_T(s.audio && s.midi && s.packets && s.zip && s.files && s.secrets && s.perf && s.dialogs && s.log && s.displays && s.serial, "member missing");
    REQUIRE_T(s.serial->health().state == AdapterState::Unavailable || s.serial->health().state == AdapterState::Stopped, "serial must not claim to work");
    return {true, "desktop adapter set complete"};
}

}  // namespace

void registerAdaptersDesktopSelfTests(SelfTestRegistry& r) {
    r.add("adapters-zip-safe", zipSafe);
    r.add("adapters-udp-loopback", udpLoopback);
    r.add("adapters-durable-fs", durableFs);
    r.add("adapters-midi-no-ports", midiNoPorts);
    r.add("adapters-audio-no-device", audioNoDevice);
    r.add("adapters-timestamp-mapper", mapper);
    r.add("adapters-secrets-no-fallback", secrets);
    r.add("adapters-log-ring", logRing);
    r.add("adapters-perf", perf);
    r.add("adapters-displays", displays);
    r.add("adapters-lighting-gate", lightingGate);
    r.add("adapters-factory", factorySet);
}

}  // namespace ss::app
