// Owner: shell-integration. "smoke" is the contract's headless proof: AppController + fake adapters for 2 s.
#include <algorithm>
#include <chrono>
#include <thread>
#include "App/AppController.h"
#include "Engine/AdapterSet.h"
#include "Engine/Fakes.h"
#include "Tests/SelfTest.h"

namespace ss::app {

namespace {

struct OutcomeCatcher final : UiModelListener {
    std::vector<ActionOutcome> got;
    void uiActionOutcome(const ActionOutcome& o) override { got.push_back(o); }
};

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
    for (int t = 0; t < timeoutMs; t += 10) {
        if (pred()) return true;
        sleepMs(10);
    }
    return pred();
}

const ActionOutcome* findOutcome(const OutcomeCatcher& c, std::uint64_t id) {
    for (const auto& o : c.got) if (o.correlationId == id) return &o;
    return nullptr;
}

bool allBlack(const PixelBuffer& p) {
    for (std::size_t i = 0; i + 3 < p.rgba.size(); i += 4)
        if (p.rgba[i] | p.rgba[i + 1] | p.rgba[i + 2]) return false;
    return true;
}

SelfTestResult smoke() {
    auto fail = [](const std::string& why) { return SelfTestResult{false, why}; };

    AdapterSet adapters = makeFakeAdapterSet(/*withSine=*/true);
    auto packets = std::dynamic_pointer_cast<RecordingPacketSink>(adapters.packets);
    auto midi = std::dynamic_pointer_cast<FakeMidiPortProvider>(adapters.midi);
    AppConfig cfg;
    cfg.headless = true;
    AppController c(adapters, cfg);
    OutcomeCatcher catcher;
    c.ui().addListener(&catcher);

    if (!c.start().ok()) return fail("start() failed");

    // Startup order (app-shell section 5).
    static const char* kStart[] = {"diag", "settings", "crashmarker", "show", "engines", "renderer", "ui",
                                   "analysis", "audio", "midi", "schedulers", "media", "perf", "ready"};
    const auto sl = c.startupLog();
    if (sl.size() != 14) return fail("startup log has " + std::to_string(sl.size()) + " steps, expected 14");
    for (std::size_t i = 0; i < sl.size(); ++i)
        if (sl[i].name != std::string("start.") + kStart[i]) return fail("startup step " + std::to_string(i) + " is " + sl[i].name);
    for (const auto& s : sl) if (s.status == StepStatus::Failed) return fail("startup step failed: " + s.name);

    // Run for 2 s with a sine flowing in through the fake audio source.
    // Wait for the facts instead of a fixed sleep (a loaded CI machine can be slow to start threads).
    if (!waitFor([&] { ss::FeatureSnapshot t; return c.latestFeatures(t) && !t.silent; }, 10000)) return fail("no FeatureSnapshot arrived");
    if (!waitFor([&] {
        c.ui().poll();
        auto sn = c.ui().snapshot();
        if (!sn || !sn->audio.signalAlive || !c.ui().latestFrame(1)) return false;
        // The first rendered frame may arrive after an earlier UI snapshot marked the not-yet-started renderer stale.
        // Require a current healthy snapshot; a worker that actually stays stalled still fails this deadline.
        return std::none_of(sn->threads.begin(), sn->threads.end(), [](const ThreadView& thread) {
            return thread.running && thread.stale;
        });
    }, 10000)) return fail("no current UiSnapshot with live audio, a rendered frame and healthy workers");

    // 1 FeatureSnapshot arrived and reflects the sine (0.5 amplitude).
    ss::FeatureSnapshot fs;
    if (!c.latestFeatures(fs)) return fail("no FeatureSnapshot arrived");
    if (fs.silent || fs.rmsDb[0] < -30.f) return fail("FeatureSnapshot does not show the sine, rmsDb=" + std::to_string(fs.rmsDb[0]));

    // 2 A frame was produced for output 1.
    c.ui().poll();
    auto frame = c.ui().latestFrame(1);
    if (!frame) return fail("no frame produced for output 1");
    if (frame->pixels.width != 800 || frame->pixels.height != 600) return fail("frame is not 800x600");
    if (allBlack(frame->pixels)) return fail("frame is all black without a blackout");
    if (frame->info.sequence == 0) return fail("frame sequence not set");

    // 3 UiSnapshot facade is populated.
    auto snap = c.ui().snapshot();
    if (!snap) return fail("no UiSnapshot published");
    if (!snap->audio.signalAlive) return fail("snapshot says audio is not alive");
    if (snap->outputs.size() != 1) return fail("snapshot outputs != 1");
    for (const auto& t : snap->threads)
        if (t.running && t.stale) return fail(t.name + " heartbeat is stale");
    if (snap->lighting.state != LightingUiState::Disarmed) return fail("lighting not disarmed by default");
    if (snap->midi.outputEnabled) return fail("midi output enabled by default");

    // 4 Action round trip through the inbox into the show engine (single writer).
    ss::Action next;
    next.actionId = ss::id(ss::ActionId::NextScene);
    next.source = ss::ActionSource::Ui;
    const auto t1 = c.ui().submitAction(next);
    if (!t1.result.queued) return fail("NextScene was not queued");
    if (!waitFor([&] { c.ui().poll(); return findOutcome(catcher, t1.correlationId) != nullptr; }, 1000))
        return fail("no outcome for NextScene");
    if (!findOutcome(catcher, t1.correlationId)->accepted) return fail("NextScene rejected: " + findOutcome(catcher, t1.correlationId)->reason);

    // 5 Safety: Arm from a non-operator source is refused; disarm latch is honoured; blackout latch beats the queue.
    ss::Action arm;
    arm.actionId = ss::id(ss::ActionId::ArmLighting);
    arm.source = ss::ActionSource::System;
    const auto t2 = c.ui().submitAction(arm);
    if (!waitFor([&] { c.ui().poll(); return findOutcome(catcher, t2.correlationId) != nullptr; }, 1000)) return fail("no outcome for Arm");
    if (findOutcome(catcher, t2.correlationId)->accepted || c.lighting().armed()) return fail("Arm from System source was accepted");

    ss::Action black;
    black.actionId = ss::id(ss::ActionId::VisualBlackout);
    black.source = ss::ActionSource::Ui;
    const auto t3 = c.ui().submitAction(black);
    if (!t3.result.latched) return fail("blackout did not set the latch at submit time");
    if (!waitFor([&] { auto f = c.ui().latestFrame(1); return f && f->info.blackout && allBlack(f->pixels); }, 1000))
        return fail("no black frame after VisualBlackout");

    ss::Action restore;
    restore.actionId = ss::id(ss::ActionId::RestoreBlackout);
    restore.source = ss::ActionSource::Ui;
    const auto t4 = c.ui().submitAction(restore);
    if (!waitFor([&] { c.ui().poll(); auto f = c.ui().latestFrame(1); return findOutcome(catcher, t4.correlationId) && f && !f->info.blackout; }, 1500))
        return fail("frames did not resume after RestoreBlackout");

    // 6 Nothing transmitted, nothing armed, no MIDI sent.
    if (packets && packets->sent.load() != 0) return fail("packets were sent while disarmed");
    if (midi && midi->out.sent.load() != 0) return fail("MIDI was sent while disabled");

    // 7 Shutdown order (app-shell section 9), no stage timed out.
    c.ui().removeListener(&catcher);
    c.stop();
    static const char* kStop[] = {"stage1.disarm", "stage2.finalise", "stage3.quiesce", "stage4.capture",
                                  "stage5.drain", "stage6.workers", "stage7.renderer", "stage8.ui"};
    const auto el = c.shutdownLog();
    if (el.size() != 8) return fail("shutdown log has " + std::to_string(el.size()) + " stages, expected 8");
    for (std::size_t i = 0; i < el.size(); ++i) {
        if (el[i].name != std::string("shutdown.") + kStop[i]) return fail("shutdown stage " + std::to_string(i) + " is " + el[i].name);
        if (el[i].timedOut) return fail(el[i].name + " timed out");
    }
    for (int i = 0; i < static_cast<int>(ThreadId::Count); ++i)
        if (c.heartbeats().running(static_cast<ThreadId>(i))) return fail(std::string(threadName(static_cast<ThreadId>(i))) + " still running after stop");

    return {true, "startup 14 steps, features rms " + std::to_string(fs.rmsDb[0]) + " dB, frame seq " +
                      std::to_string(frame->info.sequence) + ", shutdown 8 stages clean"};
}

}  // namespace

void registerShellIntegrationTests(SelfTestRegistry& r);
void registerShellSelfTests(SelfTestRegistry& r) {
    r.add("smoke", smoke);
    r.add("shell: plugin host policies", [] {
        for (const auto kind : {HostKind::Clap, HostKind::Vst3}) {
            const auto& mode = hostModeFor(kind);
            if (mode.kind() != kind || mode.clearsProcessorOutput() || !mode.passesThroughMainBus()
                || mode.ownsAudioDevice() || mode.opensAudienceWindowsAtStartup() || !mode.editorIsEmbedded())
                return SelfTestResult{false, "plugin must pass audio unchanged and leave device ownership with the host"};
        }
        const auto& standalone = hostModeFor(HostKind::Standalone);
        if (!standalone.clearsProcessorOutput() || !standalone.ownsAudioDevice())
            return SelfTestResult{false, "standalone must keep monitoring silent and own its audio device"};
        return SelfTestResult{true, "CLAP/VST3 pass-through and standalone monitoring policies"};
    });
    registerShellIntegrationTests(r);
}

}  // namespace ss::app
