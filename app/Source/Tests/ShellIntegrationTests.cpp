// Owner: shell-integration. Headless checks for the integration work: persistence, recovery, state blob, secondary mode,
// shutdown with a stuck writer, MIDI and lighting feeding, musical-feature wiring, and the allocation-free audio entry.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <juce_core/juce_core.h>
#include "App/AppController.h"
#include "App/SampleShow.h"
#include "App/ShellState.h"
#include "Engine/Fakes.h"
#include "Tests/SelfTest.h"
#include "ss/base/sha256.hpp"
#include "nlohmann/json.hpp"

namespace ss::app {

// Defined here so shared code links in every target; set by Hooks/AllocHook.cpp in the standalone executable only.
struct AllocHookApi { void (*setCounting)(bool); unsigned long long (*count)(); };
AllocHookApi* g_allocHook = nullptr;

namespace {

SelfTestResult pass(const std::string& d = {}) { return {true, d}; }
SelfTestResult fail(const std::string& d) { return {false, d}; }

bool waitFor(const std::function<bool()>& p, int ms) {
    for (int t = 0; t < ms; t += 10) { if (p()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return p();
}

struct TempDir {
    juce::File dir;
    TempDir() : dir(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ss-shell-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()))) { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
    std::string path() const { return dir.getFullPathName().toStdString(); }
};

struct Catcher final : UiModelListener {
    std::vector<ActionOutcome> actions;
    std::vector<CommandOutcome> commands;
    void uiActionOutcome(const ActionOutcome& o) override { actions.push_back(o); }
    void uiCommandOutcome(const CommandOutcome& o) override { commands.push_back(o); }
};

AppConfig headlessCfg(const std::string& dir = {}) {
    AppConfig c;
    c.headless = true; c.startAudio = false; c.dataDir = dir;
    return c;
}

bool runCommand(AppController& c, Catcher& cat, AppCommand cmd, bool* ok = nullptr, std::string* msg = nullptr) {
    std::uint64_t id = 0;
    if (!c.ui().postCommand(cmd, &id)) return false;
    const bool got = waitFor([&] {
        c.ui().poll();
        for (const auto& o : cat.commands) if (o.correlationId == id) return true;
        return false;
    }, 3000);
    if (got) for (const auto& o : cat.commands) if (o.correlationId == id) { if (ok) *ok = o.ok; if (msg) *msg = o.message; }
    return got;
}

// ---- state blob ------------------------------------------------------------------------------------------------
std::string rewrap(const std::string& payload) {
    std::string out("SSST", 4);
    out.push_back(1); out.push_back(0);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((payload.size() >> (8 * i)) & 0xff));
    out += payload;
    Sha256 h; h.update(payload);
    const auto d = h.finish();
    out.append(reinterpret_cast<const char*>(d.data()), d.size());
    return out;
}

SelfTestResult stateBlobRoundtrip() {
    StateBlobContent in;
    in.doc.name = "Blob show"; in.doc.uuid = "11111111-1111-4111-8111-111111111111";
    ss::model::SceneDef sc; sc.id = 1; sc.name = "One"; in.doc.scenes.push_back(sc);
    in.prefs = {{"ui.theme", "contrast"}, {"broadcast.host", "never-stored"}};
    auto blob = encodeStateBlob(in);
    if (!blob.ok()) return fail("encode: " + blob.error().detail);
    auto out = decodeStateBlob(blob.value().data(), blob.value().size());
    if (!out.ok()) return fail("decode: " + out.error().detail);
    if (out.value().doc.name != "Blob show" || out.value().doc.scenes.size() != 1) return fail("document did not round trip");
    if (out.value().prefs.size() != 1 || out.value().prefs[0].first != "ui.theme") return fail("only ui.* prefs may travel");
    std::string bad = blob.value();
    bad[bad.size() - 1] ^= 1;
    if (decodeStateBlob(bad.data(), bad.size()).ok()) return fail("corrupt hash accepted");
    bad = blob.value(); bad[0] = 'X';
    if (decodeStateBlob(bad.data(), bad.size()).ok()) return fail("wrong magic accepted");
    bad = blob.value(); bad[4] = 9;
    if (decodeStateBlob(bad.data(), bad.size()).ok()) return fail("newer version accepted");
    if (decodeStateBlob(blob.value().data(), 10).ok()) return fail("truncated blob accepted");
    std::string big(kStateBlobMaxBytes + 1, 'a');
    if (decodeStateBlob(big.data(), big.size()).ok()) return fail("oversize blob accepted");
    // A reference-only state (show too big for the host's per-instance limit) round trips as a reference.
    StateBlobContent ref; ref.referenceOnly = true; ref.showName = "Blob show"; ref.showUuid = in.doc.uuid; ref.showHash = "abc";
    auto rb = encodeStateBlob(ref);
    if (!rb.ok()) return fail("reference encode: " + rb.error().detail);
    auto rd = decodeStateBlob(rb.value().data(), rb.value().size());
    if (!rd.ok() || !rd.value().referenceOnly || rd.value().showName != "Blob show") return fail("reference did not round trip");
    return pass("round trip, 6 corruption cases rejected, reference form");
}

SelfTestResult truePeakOptIn() {
    // REQ-R02: the true-peak limiter is off unless the preference asks for it, and then the processed recording path uses it.
    auto run = [&](bool on, std::string& json) {
        AppConfig cfg = headlessCfg(); cfg.startAudio = true;
        AppController c(makeFakeAdapterSet(true), cfg);
        if (on) c.setPreference("dynamics.truepeak", "1");
        if (!c.start().ok()) return false;
        const bool ready = waitFor([&] { return c.analysis().dynamics().telemetry().ready; }, 4000);
        json = c.analysis().dynamics().metadataJson();
        c.stop();
        return ready;
    };
    std::string off, on;
    if (!run(false, off)) return fail("dynamics never became ready (off)");
    if (!run(true, on)) return fail("dynamics never became ready (on)");
    if (off.find("\"trueTpLimiter\":false") == std::string::npos) return fail("default session is not the sample-peak limiter");
    if (on.find("\"trueTpLimiter\":true") == std::string::npos) return fail("preference dynamics.truepeak=1 did not enable the true-peak limiter");
    return pass("sample-peak by default, true-peak with dynamics.truepeak=1");
}

SelfTestResult overridesTravelWithState() {
    // REQ-A06/A08 end to end through the app: set a tempo and key override, take the plugin state, restore it in a new
    // controller, and the override is active again without any detector input.
    std::string blob;
    {
        AppController a(makeFakeAdapterSet(false), headlessCfg());
        if (!a.start().ok()) return fail("start a");
        ss::Action bpm; bpm.actionId = ss::id(ss::ActionId::SetBpm); bpm.source = ss::ActionSource::Ui; bpm.value = 133.0;
        ss::Action key; key.actionId = ss::id(ss::ActionId::SetKey); key.source = ss::ActionSource::Ui; key.value = 9 + 12;
        a.ui().submitAction(bpm); a.ui().submitAction(key);
        if (!waitFor([&] { a.ui().poll(); auto s = a.ui().snapshot(); return s && s->show.tempoOverride && s->show.keyOverride; }, 4000)) return fail("override not active");
        auto r = a.getStateBlob();
        a.stop();
        if (!r.ok()) return fail("state blob: " + r.error().detail);
        blob = r.value();
    }
    AppController b(makeFakeAdapterSet(false), headlessCfg());
    auto st = b.setStateBlob(blob.data(), blob.size());
    if (!st.ok()) return fail("restore: " + st.error().detail);
    if (!b.start().ok()) return fail("start b");
    const bool ok = waitFor([&] {
        b.ui().poll(); auto s = b.ui().snapshot();
        return s && s->show.tempoOverride && s->show.tempo.bpm > 132.9 && s->show.tempo.bpm < 133.1 && s->show.keyOverride &&
               s->show.key.tonic == 9 && s->show.key.minor;
    }, 4000);
    b.stop();
    return ok ? pass("133 BPM and A minor are active again after restoring the state") : fail("override was not restored");
}

SelfTestResult stateBlobNeverArms() {
    AppController c(makeFakeAdapterSet(false), headlessCfg());
    if (!c.start().ok()) return fail("start");
    StateBlobContent in; in.doc.name = "Crafted"; in.doc.scenes.push_back({});
    in.doc.scenes[0].id = 1; in.doc.scenes[0].name = "S";
    auto blob = encodeStateBlob(in);
    // Craft the payload by hand: arm flags and a stream key are present in the JSON and must be ignored.
    const std::string raw = blob.value();
    const std::uint32_t len = static_cast<unsigned char>(raw[6]) | (static_cast<unsigned char>(raw[7]) << 8) | (static_cast<unsigned char>(raw[8]) << 16);
    auto j = nlohmann::json::parse(raw.substr(10, len));
    j["armed"] = true; j["lightingArmed"] = true; j["recording"] = true; j["streamKey"] = "secret-key-value";
    const std::string crafted = rewrap(j.dump());
    if (!c.setStateBlob(crafted.data(), crafted.size()).ok()) return fail("crafted blob refused");
    if (!waitFor([&] { c.ui().poll(); auto s = c.ui().snapshot(); return s && s->showFile.name == "Crafted"; }, 2000)) return fail("state was not applied");
    if (c.lighting().armed() || c.midi().outputEnabled()) return fail("restored state armed an output");
    auto s = c.ui().snapshot();
    if (s->lighting.state != LightingUiState::Disarmed) return fail("lighting not disarmed after restore");
    // A bad blob leaves the current state alone.
    std::string bad = crafted; bad[bad.size() - 3] ^= 1;
    if (c.setStateBlob(bad.data(), bad.size()).ok()) return fail("bad blob accepted");
    const auto preserved = c.ui().snapshot();
    if (!preserved || preserved->showFile.name != "Crafted") return fail("invalid state changed the active show");
    if (c.lighting().armed() || c.midi().outputEnabled()) return fail("invalid state armed an output");
    return pass("flags ignored, outputs disarmed, corrupt blob refused without changing active show");
}

// ---- recovery and persistence ---------------------------------------------------------------------------------
SelfTestResult recoveryMarker() {
    TempDir td;
    // A lock left by a process that is gone (pid far above any real one).
    LockInfo l; l.pid = 2147480000LL; l.startUnixMs = 1; l.build = "x"; l.mode = "standalone";
    td.dir.getChildFile("running.lock").replaceWithText(lockToText(l));
    Catcher cat;
    AppController c(makeFakeAdapterSet(false), headlessCfg(td.path()));
    c.ui().addListener(&cat);
    if (!c.start().ok()) return fail("start");
    if (!c.inRecovery()) return fail("stale running.lock did not enter Recovery mode");
    if (!waitFor([&] { auto s = c.ui().snapshot(); return s && s->recovery && s->recoveryInfo.active && s->show.paused; }, 2000)) {
        auto s = c.ui().snapshot();
        std::string d;
        for (auto& e : s->recentDiag) d += e.tag + ":" + e.text + "; ";
        c.ui().poll(); std::string ao; for (auto& o : cat.actions) ao += std::to_string(o.actionId) + (o.accepted ? " ok " : " refused " + o.reason + " ");
        return fail("snapshot does not show a paused Recovery mode: [" + ao + "] recovery=" + std::to_string(s->recovery) + " info=" + std::to_string(s->recoveryInfo.active) + " paused=" + std::to_string(s->show.paused) + " " + d);
    }
    if (c.lighting().armed() || c.midi().outputEnabled()) return fail("armed in Recovery");
    if (!td.dir.getChildFile("running.lock").existsAsFile()) return fail("running.lock not rewritten at ready");
    bool ok = false;
    AppCommand cmd; cmd.kind = AppCommandKind::RecoveryStartEmpty;
    if (!runCommand(c, cat, cmd, &ok) || !ok) return fail("Start empty failed");
    if (c.inRecovery()) return fail("still in Recovery after the operator chose");
    c.ui().removeListener(&cat);
    c.stop();
    if (td.dir.getChildFile("running.lock").existsAsFile()) return fail("running.lock left after a clean stop");
    // Clean run again: no recovery.
    AppController c2(makeFakeAdapterSet(false), headlessCfg(td.path()));
    c2.start();
    const bool rec = c2.inRecovery();
    c2.stop();
    return rec ? fail("clean shutdown still triggered Recovery") : pass("stale lock => paused Recovery, nothing armed, click leaves it; clean stop removes the lock");
}

SelfTestResult showPersistence() {
    TempDir td;
    const std::string file = (td.dir.getChildFile("my.stagesignal")).getFullPathName().toStdString();
    {
        Catcher cat;
        AppController c(makeFakeAdapterSet(false), headlessCfg(td.path()));
        c.ui().addListener(&cat);
        if (!c.start().ok()) return fail("start");
        ss::Action a; a.actionId = ss::id(ss::ActionId::SetMode); a.value = 2.0; a.source = ss::ActionSource::Ui;      // Hybrid
        const auto tk = c.ui().submitAction(a);
        if (!waitFor([&] { auto s = c.ui().snapshot(); return s && s->show.mode == ss::ShowMode::Hybrid; }, 4000)) {
            c.ui().poll();
            std::string why = "no outcome";
            for (auto& o : cat.actions) if (o.correlationId == tk.correlationId) why = o.accepted ? "accepted" : "refused: " + o.reason;
            auto s = c.ui().snapshot();
            return fail("mode change not applied: " + why + ", mode " + std::to_string(int(s->show.mode)) + ", queued " + std::to_string(tk.result.queued));
        }
        if (!waitFor([&] { auto s = c.ui().snapshot(); return s && s->showFile.dirty; }, 3500)) return fail("edit did not mark the show unsaved");
        AppCommand save; save.kind = AppCommandKind::SaveShowFile; save.text = file;
        bool ok = false; std::string msg;
        if (!runCommand(c, cat, save, &ok, &msg) || !ok) return fail("save failed: " + msg);
        if (!td.dir.getChildFile("my.stagesignal").existsAsFile()) return fail("file was not written");
        if (!waitFor([&] { auto s = c.ui().snapshot(); return s && !s->showFile.dirty; }, 1500)) return fail("saved show still reads as unsaved");
        c.ui().removeListener(&cat);
        c.stop();
    }
    AppController c2(makeFakeAdapterSet(false), headlessCfg(td.path()));       // settings.json remembers the last show
    if (!c2.start().ok()) return fail("restart");
    const bool hybrid = waitFor([&] { auto s = c2.ui().snapshot(); return s && s->show.mode == ss::ShowMode::Hybrid; }, 2000);
    const bool rec = c2.inRecovery();
    c2.stop();
    if (rec) return fail("unexpected Recovery");
    if (!hybrid) return fail("last show was not reloaded");
    AppConfig bad = headlessCfg(td.path());
    bad.safeMode = true;
    AppController c3(makeFakeAdapterSet(false), bad);
    c3.start();
    waitFor([&] { auto x = c3.ui().snapshot(); return x && x->showFile.name.size() > 0; }, 5000);
    auto s3 = c3.ui().snapshot();
    const bool sample = s3 && s3->show.mode == ss::ShowMode::Auto;
    c3.stop();
    return sample ? pass("saved, dirty flag follows, reloaded on restart, --safe ignores it") : fail("--safe loaded the last show");
}

// ---- secondary and shutdown ------------------------------------------------------------------------------------
SelfTestResult secondaryMode() {
    AppConfig cfg = headlessCfg(); cfg.secondary = true;
    Catcher cat;
    AppController c(makeFakeAdapterSet(false), cfg);
    c.ui().addListener(&cat);
    if (auto st = c.start(); !st.ok()) return fail("Secondary start failed: " + st.error().detail);
    ss::Action arm; arm.actionId = ss::id(ss::ActionId::ArmLighting); arm.source = ss::ActionSource::Ui;
    const auto t = c.ui().submitAction(arm);
    const bool got = waitFor([&] { c.ui().poll(); for (auto& o : cat.actions) if (o.correlationId == t.correlationId) return true; return false; }, 1500);
    bool accepted = true;
    for (auto& o : cat.actions) if (o.correlationId == t.correlationId) accepted = o.accepted;
    auto s = c.ui().snapshot();
    c.ui().removeListener(&cat);
    c.stop();
    if (!got) return fail("Secondary: no outcome for Arm within 1.5 s, snapshot " + std::string(s ? "present" : "missing") + ", inbox pending " + std::to_string(c.ui().inbox().pending()));
    if (accepted || c.lighting().armed()) return fail("Secondary accepted Arm");
    if (!s || !s->secondary) return fail("snapshot does not say Secondary");
    return pass("Secondary refuses outputs and says so");
}

SelfTestResult shutdownStuckWriter() {
    AppConfig cfg = headlessCfg();
    cfg.stage2TimeoutMs = 200;
    cfg.stage2Writer = [](const std::atomic<bool>& killed) {      // a writer waiting on a child that never exits by itself
        for (int i = 0; i < 1000 && !killed.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    AppController c(makeFakeAdapterSet(false), cfg);
    if (auto st = c.start(); !st.ok()) return fail("start failed: " + st.error().detail);
    const auto t0 = std::chrono::steady_clock::now();
    c.stop();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const auto log = c.shutdownLog();
    if (log.size() != 8) return fail("stages: " + std::to_string(log.size()));
    if (!log[1].timedOut) return fail("stage 2 timeout not logged");
    for (std::size_t i = 0; i < log.size(); ++i) if (i != 1 && log[i].timedOut) return fail(log[i].name + " timed out");
    if (ms > 4000) return fail("shutdown took " + std::to_string(ms) + " ms");
    for (int i = 0; i < static_cast<int>(ThreadId::Count); ++i)
        if (c.heartbeats().running(static_cast<ThreadId>(i))) return fail("thread still running");
    return pass("stage 2 timed out at 200 ms, children killed in stage 6 released the writer, 8 stages, " + std::to_string(int(ms)) + " ms");
}

// ---- engines fed by the controller -----------------------------------------------------------------------------
SelfTestResult midiFeedsActions() {
    AdapterSet a = makeFakeAdapterSet(false);
    auto midi = std::dynamic_pointer_cast<FakeMidiPortProvider>(a.midi);
    Catcher cat;
    AppController c(a, headlessCfg());
    c.ui().addListener(&cat);
    c.start();
    ss::model::MidiMapping m;
    m.id = 1; m.deviceIdentity = "fake-in-1"; m.deviceName = "Fake MIDI In"; m.channel = 0;
    m.type = ss::model::MidiMsgType::Note; m.number = 60; m.actionId = ss::id(ss::ActionId::NextScene);
    c.midi().driver().post([m](ss::midi::MidiEngine& e) { e.mappings().add(m); });
    c.midi().driver().wantInput("fake-in-1");
    if (!waitFor([&] { return midi->sink != nullptr; }, 2000)) return fail("input port was not opened");
    const std::uint8_t on[3] = {0x90, 60, 100};
    int sends = 0;
    const bool got = waitFor([&] {
        if (sends++ % 5 == 0) midi->sink->onMidiBytes(1, on, 3, c.clock().nowMonotonic());     // retry: the port/mapping jobs apply on the next tick
        c.ui().poll();
        for (auto& o : cat.actions) if (o.actionId == ss::id(ss::ActionId::NextScene)) return true;
        return false;
    }, 6000);
    c.ui().removeListener(&cat);
    c.stop();
    return got ? pass("note on became NextScene through the inbox") : fail("MIDI note produced no action; sink " + std::string(midi->sink ? "open" : "null") + ", inputs " + std::to_string(c.midi().driver().inputPorts()) + ", rx " + std::to_string(c.midi().driver().engine().totals().rx));
}

SelfTestResult audioLossIdle() {
    // No audio source delivers blocks: the controller must report the loss and, with the Idle policy, idle the output.
    TempDir td;
    Catcher cat;
    AdapterSet a = makeFakeAdapterSet(false);
    AppController c(a, headlessCfg(td.path()));
    c.setPreference("audio.losspolicy", "idle");
    c.ui().addListener(&cat);
    c.start();
    const bool idle = waitFor([&] { c.ui().poll(); auto sn = c.ui().snapshot(); return sn && sn->show.audioIdle && sn->show.audioLost; }, 8000);
    c.setPreference("audio.losspolicy", "continue");
    const bool cleared = waitFor([&] { c.ui().poll(); auto sn = c.ui().snapshot(); return sn && sn->show.audioLost && !sn->show.audioIdle; }, 3000);
    c.ui().removeListener(&cat);
    c.stop();
    if (!idle) {
        auto sn = c.ui().snapshot();
        return fail(std::string("loss was not confirmed into the idle state within 8 s; lost=") + (sn && sn->show.audioLost ? "1" : "0") +
                    " idle=" + (sn && sn->show.audioIdle ? "1" : "0") + " pref=" + c.pref("audio.losspolicy", "?"));
    }
    if (!cleared) return fail("switching the policy to continue did not clear the idle state");
    return pass("no audio -> lost + idle (idle policy), continue policy reports without idling");
}

SelfTestResult lightingDrivenWhenArmed() {
    AdapterSet a = makeFakeAdapterSet(false);
    auto packets = std::dynamic_pointer_cast<RecordingPacketSink>(a.packets);
    Catcher cat;
    AppController c(a, headlessCfg());
    c.ui().addListener(&cat);
    c.start();
    auto doc = makeBuiltInSampleShow();
    ss::model::FixtureProfile prof; prof.id = "dim"; prof.name = "Dimmer"; prof.channels.push_back(ss::model::FixtureChannel{});
    doc.fixtureProfiles.push_back(prof);
    ss::model::FixturePatch patch; patch.id = 1; patch.profileId = "dim"; patch.label = "Front"; patch.universe = 0; patch.startAddress = 1;
    doc.patches.push_back(patch);
    doc.lighting.destination = "127.0.0.1";
    if (!c.loadShow(doc).ok()) return fail("show with a patch was refused");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (packets->sent.load() != 0) return fail("packets sent while disarmed");
    ss::Action arm; arm.actionId = ss::id(ss::ActionId::ArmLighting); arm.source = ss::ActionSource::Ui;
    c.ui().submitAction(arm);
    if (!waitFor([&] { return packets->sent.load() > 0; }, 5000)) {
        auto s = c.ui().snapshot();
        std::string why;
        for (auto& o : cat.actions) if (o.actionId == ss::id(ss::ActionId::ArmLighting)) why += o.accepted ? "arm accepted; " : "arm refused: " + o.reason + "; ";
        return fail("armed show sent nothing; " + why + "state " + std::to_string(int(s->lighting.state)) + " engineArmed " + std::to_string(s->lighting.engineArmed) +
                    " faults " + std::to_string(s->lighting.faults) + " note " + s->lighting.note + " issues " + std::to_string(s->lighting.patchIssues.size()));
    }
    ss::Action dis; dis.actionId = ss::id(ss::ActionId::DisarmLighting); dis.source = ss::ActionSource::Ui;
    c.ui().submitAction(dis);
    waitFor([&] { return !c.lighting().armed(); }, 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto after = packets->sent.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const bool quiet = packets->sent.load() == after;
    c.ui().removeListener(&cat);
    c.stop();
    return quiet ? pass("0 packets disarmed, packets when armed, quiet after disarm") : fail("packets kept flowing after disarm");
}

SelfTestResult mediaWiring() {
    Catcher cat;
    AppController c(makeFakeAdapterSet(false), headlessCfg());
    c.ui().addListener(&cat);
    if (!c.start().ok()) return fail("start");
    bool ok = false; std::string msg;
    AppCommand bad; bad.kind = AppCommandKind::ImportMedia; bad.text = "/definitely/not/here.png"; bad.a = 0;
    if (!runCommand(c, cat, bad, &ok, &msg)) return fail("no outcome for import");
    if (ok) return fail("import of a missing file succeeded");
    if (msg.find("media worker") != std::string::npos) return fail("import still says the media worker is missing");
    AppCommand pub; pub.kind = AppCommandKind::PublishText; pub.text = "Hello room";
    if (!runCommand(c, cat, pub, &ok, &msg) || !ok) return fail("publish text failed: " + msg);
    if (!waitFor([&] { auto s = c.ui().snapshot(); return s && s->media.loadedItems >= 1; }, 3000)) return fail("published text is not a loaded item");
    ss::Action clear; clear.actionId = ss::id(ss::ActionId::TextClear); clear.source = ss::ActionSource::Ui;
    c.ui().submitAction(clear);
    if (!waitFor([&] { auto s = c.ui().snapshot(); return s && s->media.loadedItems == 0; }, 3000)) return fail("TextClear did not clear the banner");
    AppCommand cfg; cfg.kind = AppCommandKind::ConfigureMedia; cfg.a = 1; cfg.b = 0;
    if (!runCommand(c, cat, cfg, &ok, &msg) || !ok) return fail("configure failed: " + msg);
    // StartBroadcast with value 0 only asks for the summary; nothing goes live.
    ss::Action sb; sb.actionId = ss::id(ss::ActionId::StartBroadcast); sb.source = ss::ActionSource::Ui; sb.value = 0.0;
    c.ui().submitAction(sb);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto s = c.ui().snapshot();
    c.ui().removeListener(&cat);
    c.stop();
    if (s->broadcast.state == BroadcastUiState::Live) return fail("value 0 went live");
    return pass("text publish and clear, bad import refused with a cause, configure accepted, value 0 never goes live");
}

// ---- musical features ------------------------------------------------------------------------------------------
SelfTestResult phraseWiring() {
    AppController c(makeFakeAdapterSet(false), headlessCfg());
    ss::show::ShowSnapshot snap; snap.gridValid = true; snap.tempo.bpm = 128; snap.tempo.confidence = 0.9f; snap.tempo.source = ss::ValueSource::Estimated;
    ss::FeatureSnapshot f;
    ss::Ns now = 0;
    for (int bar = 0; bar < 40; ++bar) {                     // a drop on every 8th bar, a build in the bar before it
        snap.barIndex = static_cast<std::uint64_t>(bar);
        f.arcPhase = (bar % 8 == 0 && bar > 0) ? 2 : ((bar % 8 == 7) ? 1 : 0);
        now += 1875 * ss::kNsPerMs;
        c.driveMusic(snap, f, now);
    }
    auto m = c.musicView();
    if (!m.phraseLocked) return fail("phrase did not lock after 4 consistent drops");
    snap.barIndex = 47; f.arcPhase = 1;                      // last bar of the phrase while building
    c.driveMusic(snap, f, now += 1875 * ss::kNsPerMs);
    m = c.musicView();
    if (!m.preDrop || m.barInPhrase != 7) return fail("pre-drop not flagged on the last build bar");
    return pass("locked on a drop grid, pre-drop on bar 8 of the phrase");
}

SelfTestResult keyColorWiring() {
    AppController c(makeFakeAdapterSet(false), headlessCfg());
    ss::show::ShowSnapshot snap; snap.key.tonic = 0; snap.key.minor = false; snap.key.confidence = 0.95f; snap.key.source = ss::ValueSource::Estimated;
    ss::FeatureSnapshot f;
    ss::Ns now = 0;
    MusicView first = c.musicView();
    for (int i = 0; i < 200; ++i) { now += 40 * ss::kNsPerMs; c.driveMusic(snap, f, now); }       // 8 s at 25 Hz
    auto m = c.musicView();
    if (!m.keyColorValid) return fail("key colour not valid for a confident key");
    if (m.keyR == first.keyR && m.keyG == first.keyG && m.keyB == first.keyB) return fail("colour never moved off neutral");
    const unsigned char r1 = m.keyR, g1 = m.keyG, b1 = m.keyB;
    snap.key.tonic = 7;
    for (int i = 0; i < 200; ++i) { now += 40 * ss::kNsPerMs; c.driveMusic(snap, f, now); }
    m = c.musicView();
    if (m.keyR == r1 && m.keyG == g1 && m.keyB == b1) return fail("colour did not follow a new key");
    return pass("colour follows the key, smoothed");
}

SelfTestResult beatLooksWiring() {
    AppController c(makeFakeAdapterSet(false), headlessCfg());
    c.setPreference("lighting.beatlook", "1");
    ss::show::ShowSnapshot snap; snap.tempo.bpm = 120; snap.tempo.confidence = 0.9f; snap.tempo.source = ss::ValueSource::Estimated;
    snap.strobingDisabled = true;
    ss::FeatureSnapshot f;
    ss::Ns now = 0;
    float lo = 1.f, hi = 0.f;
    for (int i = 0; i < 100; ++i) {
        now += 40 * ss::kNsPerMs;
        c.driveMusic(snap, f, now);
        const auto b = c.lighting().beatLook();
        if (!b.on) return fail("beat look not switched on from the preference");
        if (b.valid) { lo = std::min(lo, b.value); hi = std::max(hi, b.value); }
    }
    const auto m = c.musicView();
    if (!m.beatValid) return fail("beat grid never became valid");
    if (hi - lo < 0.3f) return fail("modulation does not move with the beat");
    if (m.beatCapped) return fail("sine look reported a flash cap");
    return pass("beat value swings " + std::to_string(lo) + " to " + std::to_string(hi) + " at 120 bpm, strobe lock honoured");
}

// ---- real-time safety --------------------------------------------------------------------------------------------
SelfTestResult callbackNoAlloc() {
    if (!g_allocHook) return pass("skipped: no allocation hook in this binary");
    UnknownTimestampMapper mapper;
    AudioIngress ingress(mapper);
    AudioStreamFormat fmt; fmt.rateHz = 48000; fmt.channels = 2; fmt.bufferFrames = 256;
    ingress.streamOpened(fmt);
    std::vector<float> block(256 * 2, 0.25f);
    AudioBlockView v; std::uint32_t slot = 0;
    g_allocHook->setCounting(true);
    const auto before = g_allocHook->count();
    for (int i = 0; i < 10000; ++i) {
        ingress.pushBlock(block.data(), 256, 2, 48000, static_cast<ss::Ns>(i) * 5333333);
        if ((i & 3) == 3) while (ingress.pop(v, slot)) ingress.release(slot);
    }
    const auto allocs = g_allocHook->count() - before;
    g_allocHook->setCounting(false);
    if (allocs != 0) return fail(std::to_string(allocs) + " allocations in 10000 callbacks");
    return pass("0 allocations in 10000 callbacks of 256 frames");
}

}  // namespace

void registerShellIntegrationTests(SelfTestRegistry& r) {
    r.add("shell: state blob roundtrip", stateBlobRoundtrip);
    r.add("shell: state blob never arms", stateBlobNeverArms);
    r.add("shell: true-peak limiter is opt-in", truePeakOptIn);
    r.add("shell: overrides travel with the state", overridesTravelWithState);
    r.add("shell: recovery marker", recoveryMarker);
    r.add("shell: show persistence", showPersistence);
    r.add("shell: secondary mode", secondaryMode);
    r.add("shell: shutdown stuck writer", shutdownStuckWriter);
    r.add("shell: midi feeds actions", midiFeedsActions);
    r.add("shell: audio loss idle", audioLossIdle);
    r.add("shell: lighting driven when armed", lightingDrivenWhenArmed);
    r.add("shell: media wiring", mediaWiring);
    r.add("shell: phrase wiring", phraseWiring);
    r.add("shell: key colour wiring", keyColorWiring);
    r.add("shell: beat looks wiring", beatLooksWiring);
    r.add("shell: callback no alloc", callbackNoAlloc);
}

}  // namespace ss::app
