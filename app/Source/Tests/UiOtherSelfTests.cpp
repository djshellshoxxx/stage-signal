// Owner: ui-other. Headless checks for the Diagnostics, Media/Text, MIDI/Lighting, Record/Broadcast, Help tabs and the
// overlays (Show Doctor, Command Palette, Undo/A-B, Show Report). Fake adapters, no hardware, no sleeps.
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "App/AppController.h"
#include "Engine/AdapterSet.h"
#include "Engine/Fakes.h"
#include "Tests/SelfTest.h"
#include "UI/DiagnosticsPanel.h"
#include "UI/HelpPanel.h"
#include "UI/MediaTextPanel.h"
#include "UI/MidiLightingPanel.h"
#include "UI/RecordBroadcastPanel.h"
#include "UI/Help/HelpTopics.h"
#include "UI/Overlays/CommandPalette.h"
#include "UI/Overlays/OverlayText.h"
#include "UI/Overlays/ShowDoctorView.h"
#include "UI/Overlays/ShowReportView.h"
#include "UI/Overlays/UndoSnapshotBar.h"

namespace ss::app {

namespace {

SelfTestResult pass() { return SelfTestResult{true, ""}; }
SelfTestResult fail(const std::string& why) { return SelfTestResult{false, why}; }

// A snapshot with the states the tabs care about. Built by hand: the UI reads it, never the engine.
std::shared_ptr<UiSnapshot> makeSnapshot() {
    auto s = std::make_shared<UiSnapshot>();
    s->version = 1;
    s->atNs = secToNs(100);
    s->audio.deviceState = AdapterState::Running;
    s->audio.signalAlive = true;
    s->audio.rateHz = 48000;
    s->audio.channels = 2;
    s->features.inputNearFullScale = true;
    s->features.clipRunLength = 3;
    s->record.state = RecordUiState::Recording;
    s->record.elapsedMs = 61000;
    s->record.bytesWritten = 5 * 1024 * 1024;
    s->broadcast.state = BroadcastUiState::Live;
    s->broadcast.bitrateKbps = 4500;
    s->lighting.state = LightingUiState::Armed;
    s->lighting.transportConnected = true;
    s->midi.inputPorts = 2;
    s->midi.outputPorts = 1;
    s->show.undoDepth = 3;
    s->show.slots[0].used = true;
    s->show.strobingDisabled = true;
    s->outputs.push_back(OutputView{7, "Main", RouteKind::Audience, 1920, 1080, 0, 0, false});
    s->show.selectedOutput = 7;
    s->threads.push_back(ThreadView{"render", true, false, 10});
    s->adapters.push_back(AdapterRow{"audio", AdapterState::Running, ""});
    DiagEvent e;
    e.atNs = secToNs(90);
    e.level = DiagLevel::Warn;
    e.tag = "adapter.midi";
    e.text = "Port lost";
    s->recentDiag.push_back(e);
    e.level = DiagLevel::Info;
    e.tag = "start.audio";
    e.text = "Started";
    s->recentDiag.push_back(e);
    return s;
}

// Publishes the snapshot to the model and delivers it to every listener (message thread).
void feed(UiModel& m, std::shared_ptr<UiSnapshot> s) {
    m.publish(std::move(s));
    m.poll();
}

bool containsCi(const std::string& hay, const std::string& needle) {
    auto lower = [](std::string x) {
        for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return x;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

// ---- tests -----------------------------------------------------------------------------------------------

SelfTestResult copyLint() {
    const char* banned[] = {"seamless", "effortless", "unlock", "elevate", "empower", "leverage", "supercharge",
                            "powerful", "robust", "delve", "dive into", "let's", "oops", "whoops", "we're sorry",
                            "magic", "game-changing", "unleash", "in today's", "journey", "tapestry",
                            "it's worth noting", "please note that"};
    const auto topics = help::topics(palette::KeyMap::defaults());
    for (const auto& t : topics) {
        for (const std::string* text : {&t.title, &t.body}) {
            if (text->find("\xE2\x80\x94") != std::string::npos) return fail("em dash in help topic: " + t.title);
            if (text->find("\xE2\x80\x93") != std::string::npos) return fail("en dash in help topic: " + t.title);
            if (text->find('!') != std::string::npos) return fail("exclamation mark in help topic: " + t.title);
            for (const char* b : banned)
                if (containsCi(*text, b)) return fail(std::string("banned copy '") + b + "' in " + t.title);
        }
    }
    if (topics.size() < 14) return fail("fewer than the 14 spec topics");
    return pass();
}

SelfTestResult doctorBanner() {
    using doctor::Finding;
    using doctor::Severity;
    auto f = [](Severity s) { Finding x; x.severity = s; return x; };
    if (ShowDoctorView::bannerFor({}) != "Ready") return fail("empty list should read Ready");
    if (ShowDoctorView::bannerFor({f(Severity::Warning)}) != "Ready with 1 warning") return fail("one warning wording");
    if (ShowDoctorView::bannerFor({f(Severity::Warning), f(Severity::Warning)}) != "Ready with 2 warnings")
        return fail("two warnings wording");
    if (ShowDoctorView::bannerFor({f(Severity::Blocker), f(Severity::Warning)}) != "Not ready: 1 blocker")
        return fail("blocker takes priority");
    if (ShowDoctorView::bannerFor({f(Severity::Info)}) != "Ready") return fail("info does not change the banner");
    return pass();
}

SelfTestResult doctorInputFromSnapshot() {
    UiModel m;
    ShowDoctorView doc(m);
    auto s = makeSnapshot();
    doc.observe(*s);
    auto env = doc.environmentFrom(*s);
    if (!env.audio.deviceSelected || !env.audio.deviceOpen) return fail("running device not mapped to open");
    if (!env.audio.clipping || env.audio.clipRunLength != 3) return fail("clipping not mapped");
    if (!env.lighting.armed || !env.safety.strobingDisabled) return fail("lighting or strobe state not mapped");

    s->audio.deviceState = AdapterState::Unavailable;
    s->audio.signalAlive = false;
    doc.observe(*s);
    s->atNs = secToNs(112);
    doc.observe(*s);
    env = doc.environmentFrom(*s);
    if (env.audio.deviceSelected) return fail("unavailable device still selected");
    if (env.audio.silentSeconds < 11.9 || env.audio.silentSeconds > 12.1) return fail("silence duration not tracked");

    doc.runCheck(*s);
    if (!doc.hasRun() || doc.findings().empty()) return fail("check produced no findings for a missing device");
    return pass();
}

SelfTestResult paletteSearchAndConfirm() {
    UiModel m;
    CommandPaletteOverlay pal(m);
    juce::Component host;
    pal.open(host);
    pal.setQuery("blackout");
    if (pal.results().empty()) return fail("blackout query returned nothing");
    if (!pal.results().front().safetyCritical) return fail("safety action does not rank first for blackout");
    pal.setQuery("arm lighting");
    bool found = false;
    for (const auto& r : pal.results()) found = found || r.id == ss::ActionId::ArmLighting;
    if (!found) return fail("arm lighting not found");
    pal.setQuery("zzqxj");
    if (!pal.results().empty()) return fail("nonsense query should match nothing");
    pal.close();

    if (!CommandPaletteOverlay::needsConfirmation(ss::ActionId::StartRecording)) return fail("StartRecording not confirmed");
    if (!CommandPaletteOverlay::needsConfirmation(ss::ActionId::StartBroadcast)) return fail("StartBroadcast not confirmed");
    if (!CommandPaletteOverlay::needsConfirmation(ss::ActionId::ArmLighting)) return fail("ArmLighting not confirmed");
    if (CommandPaletteOverlay::needsConfirmation(ss::ActionId::StopRecording)) return fail("stop must run at once");
    if (CommandPaletteOverlay::needsConfirmation(ss::ActionId::Undo)) return fail("undo must run at once");

    // A bad value is refused before anything is submitted (app-ui 12).
    if (palette::makeAction(ss::ActionId::SetBpm, "abc").ok()) return fail("bad BPM text was accepted");
    if (palette::makeAction(ss::ActionId::SetBpm, "999").ok()) return fail("out of range BPM was accepted");
    return pass();
}

SelfTestResult undoAndFade() {
    double v = -1.0;
    if (!UndoSnapshotBar::parseFade("1.5", v) || std::fabs(v - 1.5) > 1e-9) return fail("1.5 seconds rejected");
    if (!UndoSnapshotBar::parseFade("0", v) || std::fabs(v) > 1e-9) return fail("0 (cut) rejected");
    if (UndoSnapshotBar::parseFade("11", v)) return fail("11 seconds accepted");
    if (UndoSnapshotBar::parseFade("", v)) return fail("empty fade accepted");
    if (UndoSnapshotBar::parseFade("abc", v)) return fail("text fade accepted");

    UiModel m;
    UndoSnapshotBar bar(m);
    feed(m, makeSnapshot());
    if (bar.slotWordA() != "A stored") return fail("slot A word: " + bar.slotWordA().toStdString());
    if (bar.slotWordB() != "B empty") return fail("slot B word: " + bar.slotWordB().toStdString());
    return pass();
}

SelfTestResult reportExportGuards() {
    UiModel m;
    ShowReportView rep(m);
    if (rep.includeMask() != 31u) return fail("default include mask should check all five items");
    if (rep.exportReport() != "Choose a folder first.") return fail("export without a folder not refused");
    if (rep.formatCode() != 1 || rep.rangeCode() != 1) return fail("defaults not HTML and this session");
    if (std::string(ShowReportView::privacyStatement()).find("No secrets") == std::string::npos)
        return fail("privacy statement missing");
    return pass();
}

SelfTestResult overlayHelpers() {
    if (overlay::codePointCount("h\xC3\xA9llo") != 5) return fail("code point count wrong for accented text");
    if (overlay::agoText(secToNs(100), secToNs(70)) != "30 s ago") return fail("ago text wrong");
    if (overlay::numberText(128.0) != "128") return fail("integer number text wrong");
    if (overlay::outputLabel(*makeSnapshot(), kAllOutputs) != "All visual outputs") return fail("all outputs label");
    if (overlay::outputLabel(*makeSnapshot(), 7) != "Main") return fail("output label lookup");
    return pass();
}

SelfTestResult panelsFeedWithoutCrash() {
    UiModel m;
    DiagnosticsPanel diag(m);
    MidiLightingPanel midi(m);
    RecordBroadcastPanel rec(m);
    MediaTextPanel media(m);
    HelpPanel help(m);
    if (help.topicCount() < 14) return fail("help topics missing");
    bool keyboard = false;
    for (const auto& t : help.titles()) keyboard = keyboard || t == "Keyboard map";
    if (!keyboard) return fail("keyboard map topic missing");

    // Empty snapshot first, then a populated one, then an unavailable device: none of them may crash.
    feed(m, std::make_shared<UiSnapshot>());
    feed(m, makeSnapshot());
    auto silent = makeSnapshot();
    silent->audio.deviceState = AdapterState::Unavailable;
    silent->audio.signalAlive = false;
    feed(m, silent);
    diag.setBounds(0, 0, 800, 456);
    midi.setBounds(0, 0, 800, 456);
    rec.setBounds(0, 0, 800, 456);
    media.setBounds(0, 0, 800, 456);
    help.setBounds(0, 0, 800, 456);
    return pass();
}

SelfTestResult sendsReachTheQueue() {
    AdapterSet adapters = makeFakeAdapterSet(/*withSine=*/true);
    AppConfig cfg;
    cfg.headless = true;
    AppController c(adapters, cfg);
    if (!c.start().ok()) return fail("start() failed");
    std::uint64_t corr = 0;
    const std::string undo = overlay::sendAction(c.ui(), ss::ActionId::Undo, 0.0, kAllOutputs, &corr);
    if (undo != "Sent.") return fail("undo not queued: " + undo);
    if (corr == 0) return fail("no correlation id for undo");
    const std::string arm = overlay::sendAction(c.ui(), ss::ActionId::ArmLighting, 0.0, kAllOutputs, nullptr);
    if (arm != "Sent.") return fail("arm not queued: " + arm);
    if (!overlay::sendCommand(c.ui(), AppCommand{AppCommandKind::RunShowDoctor, "", "", 0, 0, 0.0, 0})) return fail("command queue refused");
    c.stop();
    return pass();
}

SelfTestResult keyboardHelpMentionsBlackout() {
    const auto text = palette::keyboardHelpText(palette::KeyMap::defaults());
    if (text.find("F8") == std::string::npos) return fail("F8 missing from keyboard help");
    return pass();
}

}  // namespace

void registerUiOtherSelfTests(SelfTestRegistry& r) {
    r.add("ui-other: copy lint", copyLint);
    r.add("ui-other: doctor banner", doctorBanner);
    r.add("ui-other: doctor input from snapshot", doctorInputFromSnapshot);
    r.add("ui-other: palette search and confirm", paletteSearchAndConfirm);
    r.add("ui-other: undo and fade", undoAndFade);
    r.add("ui-other: report export guards", reportExportGuards);
    r.add("ui-other: overlay helpers", overlayHelpers);
    r.add("ui-other: panels feed without crash", panelsFeedWithoutCrash);
    r.add("ui-other: sends reach the queue", sendsReachTheQueue);
    r.add("ui-other: keyboard help", keyboardHelpMentionsBlackout);
}

}  // namespace ss::app
