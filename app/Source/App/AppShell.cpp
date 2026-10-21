// AppController shell services (app-contract section 6): settings, running.lock + Recovery mode, persist-backed show
// load/save/autosave, plugin state blob, Secondary mode, device caches, MIDI/lighting wiring, perf adaptation.
// Owner: shell-integration. Everything here is part of AppController; the split only keeps AppController.cpp short.
//
// Threads: CONTROL fills the UiSnapshot (shellFillUi) and applies staged state. IO runs commands, autosave and device
// refresh (shellIoTick). `Shell::m` guards every field that both read; nothing here is called from the audio thread.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <juce_core/juce_core.h>
#include "App/AppController.h"
#include "App/ShellImpl.h"
#include "App/SampleShow.h"
#include "App/ShellState.h"
#include "Engine/Desktop/UdpPacketSink.h"
#include "ss/base/sha256.hpp"
#include "ss/audio/phrase_tracker.hpp"
#include "ss/diag/show_report.hpp"
#include "ss/palette/key_color.hpp"
#include "ss/show/beat_looks.hpp"
#include "ss/diag/support_bundle.hpp"
#include "ss/lighting/engine.hpp"
#include "ss/palette/keymap.hpp"
#include "ss/palette/registry.hpp"
#include "ss/perf/adaptation.hpp"
#include "ss/persist/session.hpp"
#include "ss/persist/settings.hpp"
#include "ss/router/router.hpp"
#include "ss/security/secret_store.hpp"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <signal.h>
 #include <unistd.h>
#endif

namespace ss::app {

namespace {

bool processAlive(long long pid) {
    if (pid <= 0) return false;
#if JUCE_WINDOWS
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

long long ownPid() {
#if JUCE_WINDOWS
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

std::string hhmm(std::int64_t unixMs) {
    return juce::Time(unixMs).formatted("%H:%M").toStdString();
}

std::int64_t wallMs(const ss::IClock& c) { return c.wallUnixNs() / ss::kNsPerMs; }

std::string showHash(ss::model::ShowDocument d) {
    d.journal.clear();                                   // the engine appends journal entries; they are not edits
    auto j = ss::persist::showToJson(d);
    if (!j.ok()) return {};
    return ss::Sha256::hashHex(j.value().data(), j.value().size());
}

std::string joinText(const std::vector<std::string>& v, char sep) {
    std::string o;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) o += sep; o += v[i]; }
    return o;
}

const char* kLightingNoteSecondary = "Another StageSignal in this host owns the outputs";

}  // namespace

// ============================================================================================================
// Shell state
// ============================================================================================================

// ============================================================================================================
// Small accessors
// ============================================================================================================
std::string AppController::pref(const std::string& key, const std::string& fallback) const {
    if (!shell_) return fallback;
    std::lock_guard<std::mutex> g(shell_->m);
    return shell_->prefs.get(key, fallback);
}

void AppController::setPreference(const std::string& key, const std::string& value) {
    { std::lock_guard<std::mutex> g(shell_->m); if (!shell_->prefs.set(key, value).ok()) return; }
    shellSavePrefs();
}

MusicView AppController::musicView() const {
    std::lock_guard<std::mutex> g(shell_->m);
    return shell_->music;
}

bool AppController::hasUnsavedWork() const {
    if (!shell_) return false;
    std::lock_guard<std::mutex> g(shell_->m);
    return shell_->dirty;
}

bool AppController::mediaActive() const {
    auto s = ui_.snapshot();
    if (!s) return false;
    return s->record.state == RecordUiState::Recording || s->record.state == RecordUiState::Preparing ||
           s->broadcast.state == BroadcastUiState::Live || s->broadcast.state == BroadcastUiState::Preparing ||
           s->broadcast.state == BroadcastUiState::Reconnecting;
}

void AppController::shellSaveSettings() {
    if (shell_->dir.empty() || cfg_.secondary) return;
    ss::persist::LocalSettings copy;
    { std::lock_guard<std::mutex> g(shell_->m); copy = shell_->settings; }
    auto st = ss::persist::saveSettings(*adapters_.files, shell_->settingsPath, copy);
    if (!st.ok()) diag_.post(DiagLevel::Warn, "settings", "Settings could not be saved: " + st.error().detail);
}

void AppController::shellSavePrefs() {
    if (shell_->dir.empty() || cfg_.secondary) return;
    std::string text;
    { std::lock_guard<std::mutex> g(shell_->m); text = shell_->prefs.serialise(); }
    auto st = ss::persist::atomicWriteFile(*adapters_.files, shell_->prefsPath, text);
    if (!st.ok()) diag_.post(DiagLevel::Warn, "settings", "Preferences could not be saved: " + st.error().detail);
}

// ============================================================================================================
// Start steps 2, 3, 4, 14 and stage 8
// ============================================================================================================
ss::Status AppController::shellLoadSettings() {
    auto& sh = *shell_;
    sh.dir = cfg_.secondary ? std::string() : cfg_.dataDir;
    sh.startNs = clock().nowMonotonic();
    sh.startWallMs = wallMs(clock());
    if (sh.dir.empty()) return ss::okStatus();                  // nothing persisted in this run
    auto& fs = *adapters_.files;
    fs.createDirs(sh.dir);
    sh.settingsPath = ss::persist::joinPath(sh.dir, "settings.json");
    sh.prefsPath = ss::persist::joinPath(sh.dir, "prefs.txt");
    sh.lockPath = ss::persist::joinPath(sh.dir, "running.lock");
    sh.untitledPath = ss::persist::joinPath(ss::persist::joinPath(sh.dir, "shows"), "untitled.stagesignal");
    auto r = ss::persist::loadSettings(fs, sh.settingsPath);
    if (r.ok()) {
        sh.settings = r.value();
    } else {
        // Section 5 step 2: defaults in use, a copy of the unreadable file is kept.
        fs.rename(sh.settingsPath, sh.settingsPath + ".bad");
        diag_.post(DiagLevel::Warn, "settings", "Settings could not be read. Defaults are in use. The old file was kept as settings.json.bad.");
    }
    if (fs.exists(sh.prefsPath)) {
        auto t = fs.readFile(sh.prefsPath, PrefsStore::kMaxFileBytes);
        if (t.ok()) sh.prefs = PrefsStore::parse(t.value());
    }
    if (cfg_.safeMode) {                                         // --safe: nothing saved is applied this run
        sh.settings.deviceHints.clear();
        sh.settings.lastShowPath.clear();
        diag_.post(DiagLevel::Info, "settings", "Safe mode: saved devices and the last show are ignored.");
    }
    for (const auto& h : sh.settings.deviceHints) {
        if (h.role == "audioInput") {
            const auto bar = h.identity.find('|');
            if (bar != std::string::npos) { sh.audioType = h.identity.substr(0, bar); sh.audioName = h.identity.substr(bar + 1); }
        } else if (h.role == "midiIn") {
            sh.wantedInputs.insert(h.identity);
        } else if (h.role == "lightingNode") {
            sh.ifaceIp = h.identity;
        }
    }
    sh.endpoint.host = sh.prefs.get("broadcast.host");
    sh.endpoint.path = sh.prefs.get("broadcast.path");
    sh.endpoint.port = std::atoi(sh.prefs.get("broadcast.port", "0").c_str());
    // Keymap overrides from preferences (all-or-nothing; a bad line keeps the defaults).
    const std::string ov = sh.prefs.get("keymap.overrides");
    if (!ov.empty()) {
        std::vector<std::string> lines;
        std::size_t pos = 0;
        while (pos <= ov.size()) {
            std::size_t e = ov.find('|', pos);
            if (e == std::string::npos) e = ov.size();
            if (e > pos) lines.push_back(ov.substr(pos, e - pos));
            pos = e + 1;
        }
        if (!sh.keymap.importOverrides(lines).ok()) {
            sh.keymap = ss::palette::KeyMap::defaults();
            diag_.post(DiagLevel::Warn, "settings", "Saved key bindings were not valid. Defaults are in use.");
        }
    }
    return ss::okStatus();
}

ss::Status AppController::shellCrashMarker() {
    auto& sh = *shell_;
    if (sh.dir.empty()) return ss::okStatus();
    auto& fs = *adapters_.files;
    sh.ownsLock = true;
    if (!fs.exists(sh.lockPath)) return ss::okStatus();
    LockInfo old;
    auto txt = fs.readFile(sh.lockPath, 4096);
    const bool parsed = txt.ok() && lockFromText(txt.value(), old);
    const char* mode = cfg_.host == HostKind::Clap ? "clap" : "standalone";
    if (parsed && old.pid != ownPid() && old.mode == mode && processAlive(old.pid)) {
        // A live process owns the marker: this is not a crash. Leave its lock alone.
        sh.ownsLock = false;
        diag_.post(DiagLevel::Warn, "crashmarker", "Another StageSignal is running with the same data folder.");
        return ss::okStatus();
    }
    recovery_.store(true);
    diag_.post(DiagLevel::Warn, "crashmarker", "The last run did not end cleanly. Recovery mode: nothing is restored or armed until you choose.");
    return ss::okStatus();
}

void AppController::shellWriteLock() {
    auto& sh = *shell_;
    if (sh.dir.empty() || !sh.ownsLock) return;
    LockInfo l;
    l.pid = ownPid(); l.startUnixMs = sh.startWallMs; l.build = "0.2.0";
    l.mode = cfg_.host == HostKind::Clap ? "clap" : "standalone";
    auto st = adapters_.files->writeFile(sh.lockPath, lockToText(l), true);
    if (!st.ok()) diag_.post(DiagLevel::Warn, "crashmarker", "running.lock could not be written");
}

void AppController::shellRemoveLock() {
    auto& sh = *shell_;
    if (sh.dir.empty() || !sh.ownsLock) return;
    adapters_.files->remove(sh.lockPath);
    sh.ownsLock = false;
}

ss::Status AppController::shellOpenPath(const std::string& path) {
    auto& sh = *shell_;
    ss::persist::ShowStore store(*adapters_.files, clock(), path);
    auto r = store.load();
    if (!r.ok()) {
        std::lock_guard<std::mutex> g(sh.m);
        sh.fileError = "This show could not be opened: " + r.error().detail;
        return r.error();
    }
    auto st = loadShow(r.value().parsed.doc);
    if (!st.ok()) {
        std::lock_guard<std::mutex> g(sh.m);
        sh.fileError = "This show could not be loaded: " + st.error().detail;
        return st;
    }
    adapters_.files->createDirs(ss::persist::parentDir(path));
    auto session = std::make_unique<ss::persist::ShowSession>(*adapters_.files, clock(), path);
    session->open();
    {
        std::lock_guard<std::mutex> g(sh.m);
        if (sh.session) sh.session->closeClean();
        sh.session = std::move(session);
        sh.sessionPath = path;
        sh.savedHash = showHash(show_.document());
        sh.dirty = false;
        sh.fileError.clear();
        sh.settings.lastShowPath = path;
        for (const auto& n : r.value().notes) sh.fileError = n;      // e.g. "restored from backup 1"
    }
    shellSaveSettings();
    diag_.post(DiagLevel::Info, "show", "Show opened");
    return ss::okStatus();
}

ss::Status AppController::shellSavePath(const std::string& pathIn) {
    auto& sh = *shell_;
    std::string path = pathIn;
    ss::model::ShowDocument doc = show_.document();
    {
        std::lock_guard<std::mutex> g(sh.m);
        if (path.empty()) path = sh.sessionPath;
    }
    if (path.empty()) {
        std::lock_guard<std::mutex> g(sh.m);
        sh.fileError = "There is no place to save yet. Choose a file name.";
        return ss::makeError(ss::ErrorCategory::InvalidArgument, sh.fileError);
    }
    adapters_.files->createDirs(ss::persist::parentDir(path));
    std::lock_guard<std::mutex> g(sh.m);
    if (!sh.session || sh.sessionPath != path) {
        if (sh.session) sh.session->closeClean();
        sh.session = std::make_unique<ss::persist::ShowSession>(*adapters_.files, clock(), path);
        sh.session->open();
        sh.sessionPath = path;
    }
    auto st = sh.session->saveNow(doc);
    if (!st.ok()) {
        sh.fileError = "The show could not be saved: " + st.error().detail;
        return st;
    }
    sh.savedHash = showHash(doc);
    sh.dirty = false;
    sh.fileError.clear();
    sh.autosaveNote.clear();
    sh.settings.lastShowPath = path;
    diag_.post(DiagLevel::Info, "show", "Show saved");
    return ss::okStatus();
}

ss::Status AppController::saveShowNow() {
    auto st = shellSavePath(std::string());
    if (st.ok()) shellSaveSettings();
    return st;
}

void AppController::discardUnsavedWork() {
    if (!shell_) return;
    std::lock_guard<std::mutex> g(shell_->m);
    if (shell_->session) shell_->session->closeClean();
    shell_->dirty = false;
}

ss::Status AppController::shellLoadShow() {
    auto& sh = *shell_;
    auto sampleDoc = makeBuiltInSampleShow();
    auto loadSample = [&]() { return cfg_.useSampleShow ? loadShow(sampleDoc) : ss::okStatus(); };
    if (sh.dir.empty() && cfg_.showPath.empty()) return loadSample();

    std::string path = cfg_.showPath;
    if (path.empty()) path = sh.settings.lastShowPath;
    if (path.empty()) path = sh.untitledPath;
    auto& fs = *adapters_.files;

    // Recovery proposal: autosave newer than the saved file, or a session lock left behind.
    if (!path.empty() && fs.exists(path + ".recovery")) {
        ss::persist::ShowSession probe(fs, clock(), path);
        auto p = probe.detectRecovery();
        if (p.ok() && p.value().has_value()) {
            sh.proposal = *p.value();
            sh.haveProposal = true;
            recovery_.store(true);
        }
    }
    if (recovery_.load()) {
        auto st = loadSample();                                  // idle: the sample show, paused; nothing from disk
        sh.sessionPath = path;
        sh.recoveryView.active = true;
        sh.recoveryView.lastSavedAvailable = fs.exists(path);
        sh.recoveryView.autosaveAvailable = sh.haveProposal;
        if (sh.haveProposal) sh.recoveryView.autosaveTime = hhmm(sh.proposal.recoveryWallUnixMs);
        sh.recoveryView.text = std::string("The last session did not end cleanly. The sample show is loaded and paused. Lighting, MIDI output, ") +
                               "recording and broadcast are off. Nothing was restored automatically.";
        if (sh.haveProposal) sh.recoveryView.text += " An autosave from " + sh.recoveryView.autosaveTime + " is available.";
        recoveryPausePending_.store(true);                       // CONTROL pauses the show once it is ticking
        return st;
    }

    if (fs.exists(path)) {
        auto st = shellOpenPath(path);
        if (st.ok()) return st;
        diag_.post(DiagLevel::Warn, "show", "The last show could not be opened. The sample show is loaded instead.");
        std::string err;
        { std::lock_guard<std::mutex> g(sh.m); err = sh.fileError; }
        auto st2 = loadSample();
        std::lock_guard<std::mutex> g(sh.m);
        sh.fileError = err;
        return st2;
    }
    // No file yet: start from the sample show, bound to a working file so autosave and Save work.
    auto st = loadSample();
    if (!path.empty() && !cfg_.secondary) {
        fs.createDirs(ss::persist::parentDir(path));
        sh.session = std::make_unique<ss::persist::ShowSession>(fs, clock(), path);
        sh.session->open();
        sh.sessionPath = path;
        sh.savedHash = showHash(show_.document());
    }
    return st;
}

// ============================================================================================================
// Engine configuration from the document (MIDI mappings, lighting patch)
// ============================================================================================================
void AppController::shellConfigureEngines(const ss::model::ShowDocument& doc) {
    auto& sh = *shell_;
    ss::model::LightingSettings ls;
    std::vector<ss::model::MidiMapping> maps;
    {
        std::lock_guard<std::mutex> g(sh.m);
        sh.doc = doc;
        sh.showName = doc.name;
        sh.scenes.clear();
        for (const auto& s : doc.scenes) sh.scenes.push_back({s.id, s.name, s.strobe});
        sh.lighting = doc.lighting;
        // Prefs override the show's destination only when the operator typed one on this machine.
        const std::string dest = sh.prefs.get("lighting.destination");
        if (!dest.empty()) sh.lighting.destination = dest;
        if (sh.prefs.get("lighting.broadcast") == "1") sh.lighting.broadcast = true;
        ls = sh.lighting;
        sh.mappings = doc.midi;
        maps = sh.mappings;
        // Patch report for the MIDI / Lighting tab (pure function, no engine access).
        sh.patchIssues.clear();
        auto report = ss::lighting::validatePatches(doc.fixtureProfiles, doc.patches, ls.protocol);
        for (const auto& i : report.issues) sh.patchIssues.push_back(i.message);
        sh.patchCanArm = report.canArm();
        std::set<std::uint16_t> u;
        for (const auto& p : doc.patches) u.insert(p.universe);
        sh.universes = static_cast<int>(u.size());
        sh.fixtures = static_cast<int>(doc.patches.size());
        for (const auto& m : maps) if (!m.deviceName.empty()) {}    // device names are matched by the engine itself
    }
    lighting_.setRateHz(ls.rateHz);
    shellPushMediaConfig();
    // The cid is per installation: kept in prefs so the sACN source identity is stable across runs.
    ss::lighting::sacn::Cid cid{};
    {
        std::lock_guard<std::mutex> g(sh.m);
        std::string hex = sh.prefs.get("lighting.cid");
        if (hex.size() != 32) {
            juce::Random rnd;
            hex.clear();
            for (int i = 0; i < 16; ++i) { char b[3]; std::snprintf(b, sizeof b, "%02x", rnd.nextInt(256)); hex += b; }
            sh.prefs.set("lighting.cid", hex);
        }
        for (int i = 0; i < 16; ++i) cid[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::strtol(hex.substr(static_cast<std::size_t>(i) * 2, 2).c_str(), nullptr, 16));
        bool allZero = true;
        for (auto b : cid) if (b) allZero = false;
        if (allZero) cid[0] = 1;
    }
    auto profiles = doc.fixtureProfiles;
    auto patches = doc.patches;
    lighting_.disarm();                                          // a new patch or destination always starts disarmed
    lighting_.driver().post([profiles, patches, ls, cid](ss::lighting::LightingEngine& e) {
        auto cfg = ss::lighting::configFromSettings(ls, cid, "StageSignal");
        if (!cfg.ok()) return;
        e.configure(profiles, patches, cfg.value());
    });
    { std::vector<std::uint32_t> ids; for (const auto& p : patches) ids.push_back(p.id); lighting_.driver().setBeatFixtures(std::move(ids)); }
    midi_.driver().post([maps](ss::midi::MidiEngine& e) { e.mappings().load(maps); });
    if (auto* udp = dynamic_cast<UdpPacketSink*>(adapters_.packets.get())) udp->setBroadcastEnabled(ls.broadcast);
}

// ============================================================================================================
// Wiring: MIDI in -> actions, command handlers
// ============================================================================================================
void AppController::shellInstallWiring() {
    // MIDI engine actions enter the same inbox as the UI (source Midi); safety actions set latches at submit time.
    midi_.driver().setActionSink([this](const ss::Action& a) {
        ss::Action x = a;
        x.source = ss::ActionSource::Midi;
        ui_.submitAction(std::move(x));
    });
    // Ctrl+S and the palette's "Save show" arrive as actions; saving blocks, so it runs on the IO thread as a command.
    registerActionHandler(ss::ActionId::SaveShow, [this](const ss::Action&) {
        AppCommand c; c.kind = AppCommandKind::SaveShowFile;
        ss::ActionResult r;
        r.accepted = ui_.postCommand(c);
        if (!r.accepted) r.reason = "Too many requests waiting. Try again in a moment.";
        return r;
    });
    registerActionHandler(ss::ActionId::OpenCommandPalette, [](const ss::Action&) { ss::ActionResult r; r.accepted = true; return r; });
    shellRegisterCommands();
}

void AppController::shellRegisterCommands() {
    auto reject = [](const std::string& why) { return ss::Status(ss::makeError(ss::ErrorCategory::InvalidArgument, why)); };
    auto& sh = *shell_;

    // ---- show files and recovery ---------------------------------------------------------------------------------
    registerCommandHandler(AppCommandKind::LoadShowFile, [this, reject](const AppCommand& c) {
        if (c.text.empty()) return reject("Choose a file first.");
        return shellOpenPath(c.text);
    });
    registerCommandHandler(AppCommandKind::SaveShowFile, [this](const AppCommand& c) {
        auto st = shellSavePath(c.text);
        if (st.ok()) shellSaveSettings();
        return st;
    });
    registerCommandHandler(AppCommandKind::NewShow, [this](const AppCommand&) {
        auto st = loadShow(makeBuiltInSampleShow());
        if (st.ok()) {
            std::lock_guard<std::mutex> g(shell_->m);
            shell_->savedHash.clear();                           // differs from anything saved, so it reads as unsaved
            shell_->dirty = shell_->session != nullptr;
        }
        return st;
    });
    auto leaveRecovery = [this] {
        recovery_.store(false);
        std::lock_guard<std::mutex> g(shell_->m);
        shell_->recoveryView = RecoveryView{};
        shell_->haveProposal = false;
    };
    registerCommandHandler(AppCommandKind::RecoveryRestoreAutosave, [this, reject, leaveRecovery](const AppCommand&) {
        auto& s = *shell_;
        ss::persist::RestoreProposal p;
        std::string path;
        { std::lock_guard<std::mutex> g(s.m); if (!s.haveProposal) return reject("There is no autosave to restore."); p = s.proposal; path = s.sessionPath; }
        ss::persist::ShowSession sess(*adapters_.files, clock(), path);
        auto r = sess.acceptRestore(p);
        if (!r.ok()) return ss::Status(r.error());
        auto st = shellOpenPath(p.restoredCopyPath);
        if (st.ok()) leaveRecovery();
        return st;
    });
    registerCommandHandler(AppCommandKind::RecoveryOpenLast, [this, reject, leaveRecovery](const AppCommand&) {
        auto& s = *shell_;
        std::string path;
        { std::lock_guard<std::mutex> g(s.m); path = s.sessionPath; }
        if (path.empty() || !adapters_.files->exists(path)) return reject("There is no saved show to open.");
        ss::persist::ShowSession sess(*adapters_.files, clock(), path);
        sess.discardRestore();
        auto st = shellOpenPath(path);
        if (st.ok()) leaveRecovery();
        return st;
    });
    registerCommandHandler(AppCommandKind::RecoveryStartEmpty, [this, leaveRecovery](const AppCommand&) {
        auto& s = *shell_;
        std::string path;
        { std::lock_guard<std::mutex> g(s.m); path = s.sessionPath; }
        if (!path.empty()) { ss::persist::ShowSession sess(*adapters_.files, clock(), path); sess.discardRestore(); }
        auto st = loadShow(makeBuiltInSampleShow());
        if (st.ok()) {
            ss::Action resume;
            resume.actionId = ss::id(ss::ActionId::Resume);
            resume.source = ss::ActionSource::System;
            ui_.submitAction(resume);
            std::lock_guard<std::mutex> g(s.m);
            s.savedHash = showHash(show_.document());
            s.dirty = false;
            if (!path.empty()) {
                s.session = std::make_unique<ss::persist::ShowSession>(*adapters_.files, clock(), path);
                s.session->open();
            }
        }
        leaveRecovery();
        return st;
    });

    // ---- preferences ---------------------------------------------------------------------------------------------
    registerCommandHandler(AppCommandKind::SetPref, [this, reject](const AppCommand& c) {
        const bool ok = c.text.rfind("ui.", 0) == 0 || c.text.rfind("display.", 0) == 0 || c.text.rfind("wizard.", 0) == 0 || c.text == "lighting.beatlook" ||
                        c.text == "keymap.overrides";
        if (!ok) return reject("That setting cannot be changed here.");
        {
            std::lock_guard<std::mutex> g(shell_->m);
            auto st = shell_->prefs.set(c.text, c.text2);
            if (!st.ok()) return st;
        }
        shellSavePrefs();
        return ss::okStatus();
    });

    // ---- audio ---------------------------------------------------------------------------------------------------
    registerCommandHandler(AppCommandKind::SelectAudioDevice, [this, reject](const AppCommand& c) {
        if (cfg_.secondary) return reject("The host owns the audio device.");
        if (c.text2.empty()) return reject("Enter the device name first.");
        AudioDeviceDesc d; d.typeName = c.text; d.deviceName = c.text2;
        auto st = adapters_.audio->selectDevice(d);
        {
            std::lock_guard<std::mutex> g(shell_->m);
            shell_->audioType = c.text; shell_->audioName = c.text2;
            auto& hints = shell_->settings.deviceHints;
            hints.erase(std::remove_if(hints.begin(), hints.end(), [](const ss::persist::DeviceHint& h) { return h.role == "audioInput"; }), hints.end());
            hints.push_back({"audioInput", c.text + "|" + c.text2, c.text2});
            shell_->audioScanned = false;                        // rescan on the next IO tick
        }
        shellSaveSettings();
        return st;
    });
    registerCommandHandler(AppCommandKind::SetAudioRate, [](const AppCommand&) {
        return ss::Status(ss::makeError(ss::ErrorCategory::UnsupportedCapability,
                                        "The sample rate follows the device in this build. Change it in the system sound settings."));
    });

    // ---- lighting ------------------------------------------------------------------------------------------------
    registerCommandHandler(AppCommandKind::SelectLightingInterface, [this, reject](const AppCommand& c) {
        if (cfg_.secondary) return reject(kLightingNoteSecondary);
        auto ip = ss::lighting::parseIpv4(c.text);
        if (!ip.ok()) return reject("That is not a valid IPv4 address.");
        lighting_.disarm();
        auto st = adapters_.packets->bindInterface(c.text);
        {
            std::lock_guard<std::mutex> g(shell_->m);
            if (st.ok()) {
                shell_->ifaceIp = c.text;
                auto& hints = shell_->settings.deviceHints;
                hints.erase(std::remove_if(hints.begin(), hints.end(), [](const ss::persist::DeviceHint& h) { return h.role == "lightingNode"; }), hints.end());
                hints.push_back({"lightingNode", c.text, "Lighting interface"});
            }
        }
        if (st.ok()) shellSaveSettings();
        return st;
    });
    registerCommandHandler(AppCommandKind::SetLightingDestination, [this, reject](const AppCommand& c) {
        if (cfg_.secondary) return reject(kLightingNoteSecondary);
        auto ip = ss::lighting::parseIpv4(c.text);
        if (!ip.ok()) return reject("That is not a valid IPv4 address.");
        ss::model::ShowDocument doc;
        {
            std::lock_guard<std::mutex> g(shell_->m);
            const bool sacn = shell_->lighting.protocol == ss::model::LightingProtocol::Sacn;
            if (c.a != 0 && c.a != (sacn ? 5568 : 6454))
                return reject(std::string("The port is fixed by the protocol: ") + (sacn ? "5568" : "6454") + ".");
            shell_->prefs.set("lighting.destination", c.text);
            doc = shell_->doc;
        }
        shellSavePrefs();
        shellConfigureEngines(doc);                              // disarms, then reconfigures the engine
        return ss::okStatus();
    });
    registerCommandHandler(AppCommandKind::SetLightingBroadcast, [this, reject](const AppCommand& c) {
        if (cfg_.secondary) return reject(kLightingNoteSecondary);
        ss::model::ShowDocument doc;
        {
            std::lock_guard<std::mutex> g(shell_->m);
            shell_->prefs.set("lighting.broadcast", c.a ? "1" : "0");
            doc = shell_->doc;
        }
        shellSavePrefs();
        shellConfigureEngines(doc);
        return ss::okStatus();
    });

    // ---- MIDI ----------------------------------------------------------------------------------------------------
    registerCommandHandler(AppCommandKind::OpenMidiPort, [this, reject](const AppCommand& c) {
        if (cfg_.secondary) return reject(kLightingNoteSecondary);
        if (c.text.empty()) return reject("Choose a MIDI port first.");
        if (c.a == 1) {
            midi_.driver().wantInput(c.text);
            std::lock_guard<std::mutex> g(shell_->m);
            shell_->wantedInputs.insert(c.text);
            auto& hints = shell_->settings.deviceHints;
            hints.push_back({"midiIn", c.text, c.text});
        } else {
            auto r = adapters_.midi->openOutput(c.text);
            if (!r.ok()) return ss::Status(r.error());
            midi_.setOutputPort(r.value());
            std::lock_guard<std::mutex> g(shell_->m);
            shell_->outPort = r.value(); shell_->openOutputIdentity = c.text;
        }
        shellSaveSettings();
        return ss::okStatus();
    });
    registerCommandHandler(AppCommandKind::CloseMidiPort, [this](const AppCommand& c) {
        std::lock_guard<std::mutex> g(shell_->m);
        if (c.a == 1) {
            midi_.driver().unwantInput(c.text);
            shell_->wantedInputs.erase(c.text);
            auto& hints = shell_->settings.deviceHints;
            hints.erase(std::remove_if(hints.begin(), hints.end(), [&](const ss::persist::DeviceHint& h) { return h.role == "midiIn" && h.identity == c.text; }), hints.end());
        } else if (shell_->outPort) {
            midi_.disableOutput();
            midi_.setOutputPort(nullptr);
            adapters_.midi->closeOutput(shell_->outPort);
            shell_->outPort = nullptr; shell_->openOutputIdentity.clear();
        }
        return ss::okStatus();
    });
    registerCommandHandler(AppCommandKind::EnableMidiOutput, [this, reject](const AppCommand&) {
        if (cfg_.secondary) return reject(kLightingNoteSecondary);
        { std::lock_guard<std::mutex> g(shell_->m); if (!shell_->outPort) return reject("Open a MIDI output port first."); }
        midi_.enableOutput();
        return ss::okStatus();
    });
    registerCommandHandler(AppCommandKind::MidiLearnBegin, [this](const AppCommand&) {
        { std::lock_guard<std::mutex> g(shell_->m); shell_->learn = {true, false, "Waiting for a MIDI message. Move a control."}; shell_->learnPoll = true; }
        midi_.driver().post([](ss::midi::MidiEngine& e) { e.learn().begin(); });
        return ss::okStatus();
    });
    registerCommandHandler(AppCommandKind::MidiLearnCancel, [this](const AppCommand&) {
        { std::lock_guard<std::mutex> g(shell_->m); shell_->learn = {}; shell_->learnPoll = false; }
        midi_.driver().post([](ss::midi::MidiEngine& e) { e.learn().cancel(); });
        return ss::okStatus();
    });
    registerCommandHandler(AppCommandKind::MidiLearnCommit, [this](const AppCommand& c) {
        const auto action = static_cast<ss::ActionId>(c.a);
        const auto mode = static_cast<ss::model::EncoderMode>(c.b);
        if (!ss::palette::Registry::find(action)) return ss::Status(ss::makeError(ss::ErrorCategory::InvalidArgument, "Choose an action first."));
        Shell* s = shell_.get();
        midi_.driver().post([s, action, mode](ss::midi::MidiEngine& e) {
            auto m = e.learn().commit(action, mode);
            if (!m.ok()) return;
            auto id = e.mappings().add(m.value());
            if (!id.ok()) return;
            std::lock_guard<std::mutex> g(s->m);
            s->mappings = e.mappings().toModel();
            s->learn = {};
            s->learnPoll = false;
        });
        return ss::okStatus();
    });
    registerCommandHandler(AppCommandKind::MidiRemoveMapping, [this](const AppCommand& c) {
        Shell* s = shell_.get();
        const auto id = static_cast<std::uint32_t>(c.a);
        midi_.driver().post([s, id](ss::midi::MidiEngine& e) {
            e.mappings().remove(id);
            std::lock_guard<std::mutex> g(s->m);
            s->mappings = e.mappings().toModel();
        });
        return ss::okStatus();
    });

    // ---- broadcast endpoint and key --------------------------------------------------------------------------------
    registerCommandHandler(AppCommandKind::SetBroadcastEndpoint, [this, reject](const AppCommand& c) {
        if (c.a < 0 || c.a > 65535) return reject("The port must be between 1 and 65535.");
        {
            std::lock_guard<std::mutex> g(shell_->m);
            auto a = shell_->prefs.set("broadcast.host", c.text);
            auto b = shell_->prefs.set("broadcast.path", c.text2);
            auto p = shell_->prefs.set("broadcast.port", std::to_string(c.a));
            if (!a.ok() || !b.ok() || !p.ok()) return reject("That endpoint is not valid.");
            shell_->endpoint.host = c.text; shell_->endpoint.path = c.text2; shell_->endpoint.port = static_cast<int>(c.a);
        }
        shellSavePrefs();
        shellPushMediaConfig();
        return ss::okStatus();
    });
    registerCommandHandler(AppCommandKind::SetBroadcastKey, [this, reject](const AppCommand& c) {
        if (c.text.empty()) return reject("Enter the stream key first.");
        auto st = adapters_.secrets->put(ss::security::SecretRef{"secret:broadcast-key"}, c.text);
        if (!st.ok()) return st;
        std::lock_guard<std::mutex> g(shell_->m);
        shell_->endpoint.keySaved = true;
        auto& refs = shell_->settings.secretRefs;
        bool have = false;
        for (const auto& r : refs) if (r.ref == "secret:broadcast-key") have = true;
        if (!have) refs.push_back({"Broadcast key", "secret:broadcast-key"});
        return ss::okStatus();                                   // the worker resolves "ref:broadcast-key" itself at session start
    });

    // ---- media and text: the engine is the media worker's. Said plainly instead of pretending. -------------------
    auto needsMedia = [](const char* what) {
        return [what](const AppCommand&) {
            return ss::Status(ss::makeError(ss::ErrorCategory::UnsupportedCapability, std::string(what) + " needs the media worker, which is not in this build yet."));
        };
    };
    (void) needsMedia;
    registerCommandHandler(AppCommandKind::PublishText, [this, reject](const AppCommand& c) {
        if (cfg_.secondary) return reject(kLightingNoteSecondary);
        if (c.text.empty()) return reject("Type the message first.");
        return media_.setTextBanner(c.text);
    });
    registerCommandHandler(AppCommandKind::ImportMedia, [this, reject](const AppCommand& c) {
        if (cfg_.secondary) return reject(kLightingNoteSecondary);
        if (c.a < 0 || c.a > 2) return reject("Three images at most.");
        std::string note;
        auto st = media_.importImage(static_cast<std::size_t>(c.a), c.text, &note);
        if (!st.ok()) return st;
        diag_.post(DiagLevel::Info, "media", note.empty() ? "Image imported" : note);
        return st;
    });
    registerCommandHandler(AppCommandKind::RemoveMedia, [this, reject](const AppCommand& c) {
        if (c.a < 0 || c.a > 2) return reject("Pick an image slot between 0 and 2.");
        media_.layers().clearSlot(static_cast<std::size_t>(c.a));
        return ss::okStatus();
    });
    registerActionHandler(ss::ActionId::TextClear, [this](const ss::Action&) {
        media_.layers().clearSlot(MediaLayers::kMax - 1);               // the text banner slot
        ss::ActionResult r; r.accepted = true; return r;
    });
    registerCommandHandler(AppCommandKind::SetRecordFolder, [this, reject](const AppCommand& c) {
        if (c.text.empty()) return reject("Choose a folder first.");
        if (!adapters_.files->folderWritable(c.text)) return reject("That folder cannot be written to.");
        {
            std::lock_guard<std::mutex> g(shell_->m);
            shell_->settings.recordingFolder = c.text;
        }
        shellSaveSettings();
        shellPushMediaConfig();
        return ss::okStatus();
    });
    registerCommandHandler(AppCommandKind::SetFfmpegPath, [this](const AppCommand& c) {
        { std::lock_guard<std::mutex> g(shell_->m); shell_->prefs.set("media.ffmpeg", c.text); }
        shellSavePrefs();
        shellPushMediaConfig();
        return ss::okStatus();
    });
    registerCommandHandler(AppCommandKind::ConfigureMedia, [this, reject](const AppCommand& c) {
        if (!c.text2.empty() && !adapters_.files->folderWritable(c.text2)) return reject("That folder cannot be written to.");
        {
            std::lock_guard<std::mutex> g(shell_->m);
            shell_->prefs.set("media.ffmpeg", c.text);
            if (!c.text2.empty()) shell_->settings.recordingFolder = c.text2;
            if (c.a > 0) shell_->prefs.set("media.output", std::to_string(c.a));
            shell_->prefs.set("media.video", c.b ? "1" : "0");
        }
        shellSavePrefs(); shellSaveSettings();
        shellPushMediaConfig();
        return ss::okStatus();
    });

    // ---- reports (22, 23, 24) ------------------------------------------------------------------------------------
    registerCommandHandler(AppCommandKind::RunShowDoctor, [](const AppCommand&) { return ss::okStatus(); });   // the check runs in the UI from the snapshot
    registerCommandHandler(AppCommandKind::ExportShowReport, [this, reject](const AppCommand& c) {
        if (c.text.empty()) return reject("Choose a folder first.");
        if (!adapters_.files->folderWritable(c.text)) return reject("That folder cannot be written to.");
        const std::uint32_t mask = static_cast<std::uint32_t>(c.a);
        const int format = static_cast<int>(c.b), range = static_cast<int>(c.value);
        auto events = journal_.snapshot();
        std::vector<ss::diag::FeatureSample> samples;
        std::vector<ss::diag::SceneName> names;
        {
            std::lock_guard<std::mutex> g(shell_->m);
            samples = shell_->history.samples();
            for (const auto& s : shell_->scenes) names.push_back({s.id, s.name});
        }
        if (range == 3) {                                        // custom range: "from|to" in seconds since start
            const auto bar = c.text2.find('|');
            char* e1 = nullptr; char* e2 = nullptr;
            const double lo = std::strtod(c.text2.c_str(), &e1);
            const double hi = bar == std::string::npos ? 0.0 : std::strtod(c.text2.c_str() + bar + 1, &e2);
            if (bar == std::string::npos || e1 == c.text2.c_str() || e2 == c.text2.c_str() + bar + 1 || hi <= lo)
                return reject("Enter the start and end as seconds since the app started, for example 0 and 600.");
            const ss::Ns base = shell_->startNs;
            auto outside = [&](ss::Ns t) { const double s = static_cast<double>(t - base) / 1e9; return s < lo || s > hi; };
            events.erase(std::remove_if(events.begin(), events.end(), [&](const ss::diag::Event& ev) { return outside(ev.monoNs); }), events.end());
            samples.erase(std::remove_if(samples.begin(), samples.end(), [&](const ss::diag::FeatureSample& sm) { return outside(sm.monoNs); }), samples.end());
        }
        ss::diag::ShowReportBuilder b;
        b.setTitle("StageSignal show report").setAppVersion("0.2.0").setGeneratedAtWallMs(wallMs(clock()));
        b.setSceneNames(names).addEvents(events).addSamples(samples);
        auto rep = b.build();
        if (!rep.ok()) return ss::Status(rep.error());
        const std::string stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S").toStdString();
        const std::string base = ss::persist::joinPath(c.text, "stagesignal-report-" + stamp);
        auto write = [&](const std::string& suffix, const std::string& text) {
            return ss::persist::atomicWriteFile(*adapters_.files, base + suffix, text);
        };
        ss::Status st = ss::okStatus();
        if (format == 1 || format == 3) st = write(".html", rep.value().html);
        if (st.ok() && (format == 2 || format == 3)) {
            st = write("-events.csv", rep.value().eventsCsv);
            if (st.ok()) st = write("-setlist.csv", rep.value().setlistCsv);
        }
        (void) mask;                                             // include toggles shape the sections inside the builder's defaults
        return st;
    });
    registerCommandHandler(AppCommandKind::ExportSupportBundle, [this, reject](const AppCommand& c) {
        if (c.text.empty()) return reject("Choose a folder first.");
        if (!adapters_.files->folderWritable(c.text)) return reject("That folder cannot be written to.");
        ss::diag::SupportBundleComposer comp;
        std::string info = "StageSignal 0.2.0\nhost: " + std::string(hostMode().name()) + "\n";
        comp.addItem(ss::diag::BundleCategory::AppInfo, "app-info.txt", info);
        std::string ev;
        for (const auto& e : journal_.snapshot()) ev += std::string(ss::diag::kindName(e.kind)) + " " + e.detail + "\n";
        comp.addItem(ss::diag::BundleCategory::EventJournal, "events.txt", ev);
        std::string st;
        for (const auto& r : startupLog()) st += r.name + " " + (r.status == StepStatus::Failed ? "failed" : r.status == StepStatus::Stubbed ? "stubbed" : "done") + "\n";
        comp.addItem(ss::diag::BundleCategory::SystemSummary, "startup.txt", st);
        {
            std::lock_guard<std::mutex> g(shell_->m);
            std::string shape = "name: " + shell_->showName + "\nscenes: " + std::to_string(shell_->scenes.size()) +
                                "\nuniverses: " + std::to_string(shell_->universes) + "\nfixtures: " + std::to_string(shell_->fixtures) + "\n";
            comp.addItem(ss::diag::BundleCategory::ShowStructure, "show-structure.txt", shape);
        }
        ss::security::Redactor red;
        for (const auto& ref : adapters_.secrets->list()) {
            auto v = adapters_.secrets->get(ref);
            if (v.ok()) red.addLiteral(v.value());               // secrets never leave the machine, even in a "redacted" bundle
        }
        comp.setRedactor(red);
        auto bundle = comp.compose();
        if (!bundle.ok()) return ss::Status(bundle.error());
        std::vector<ZipMember> members;
        for (const auto& f : bundle.value().files) members.push_back({f.name, f.content});
        const std::string stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S").toStdString();
        return adapters_.zip->write(ss::persist::joinPath(c.text, "stagesignal-support-" + stamp + ".zip"), members);
    });

    // ---- ArmLighting: bind the saved interface lazily so nothing opens a socket at idle ---------------------------
    registerCommandHandler(AppCommandKind::ArmLighting, [this, reject](const AppCommand&) {
        if (cfg_.secondary) return reject(kLightingNoteSecondary);
        return lighting_.arm();
    });
    (void) sh;
}

void AppController::shellPushMediaConfig() {
    MediaConfig mc;
    {
        std::lock_guard<std::mutex> g(shell_->m);
        mc.ffmpegPath = shell_->prefs.get("media.ffmpeg");
        mc.outputDir = shell_->settings.recordingFolder;
        mc.showName = shell_->showName.empty() ? std::string("show") : shell_->showName;
        mc.recordVideo = shell_->prefs.get("media.video", "1") == "1";
        const int out = std::atoi(shell_->prefs.get("media.output", "1").c_str());
        mc.programOutput = static_cast<ss::OutputId>(out > 0 ? out : 1);
        mc.broadcast.host = shell_->endpoint.host;
        mc.broadcast.path = shell_->endpoint.path;
        mc.broadcast.port = static_cast<std::uint32_t>(shell_->endpoint.port > 0 ? shell_->endpoint.port : 0);
        mc.broadcast.streamKey.handle = "ref:broadcast-key";
    }
    media_.configure(mc);
}

// ============================================================================================================
// IO thread tick: dirty detection, autosave, device caches, learn polling, free space
// ============================================================================================================
void AppController::shellIoTick(ss::Ns now) {
    auto& sh = *shell_;
    if (!sh.ready.load()) return;
    // --- dirty detection (once per second, only when the show revision moved) and 30 s autosave
    if (now >= sh.nextDirtyCheck) {
        sh.nextDirtyCheck = now + ss::kNsPerSec;
        auto snap = showSnap_.get();
        bool check = false;
        { std::lock_guard<std::mutex> g(sh.m); check = sh.session && snap && snap->revision != sh.lastCheckedRev; if (check) sh.lastCheckedRev = snap->revision; }
        if (check) {
            const std::string h = showHash(show_.document());
            std::lock_guard<std::mutex> g(sh.m);
            if (sh.session && h != sh.savedHash) { sh.dirty = true; sh.session->noteChanged(); }
            else sh.dirty = false;
        }
    }
    {
        std::lock_guard<std::mutex> g(sh.m);
        if (sh.session && sh.dirty && !recovery_.load()) {
            const bool pendingBefore = sh.session->autosavePending();
            const bool ran = sh.session->tickAutosave([&](ss::model::ShowDocument& d, ss::persist::JsonWriteOptions&) { d = show_.document(); });
            if (ran) {
                if (sh.session->lastError()) sh.autosaveNote = "Autosave failed: " + sh.session->lastError()->detail;
                else sh.autosaveNote = "Autosaved " + hhmm(wallMs(clock()));
            }
            (void) pendingBefore;
        }
    }
    // --- device caches
    if (now >= sh.nextPorts) {
        sh.nextPorts = now + 2 * ss::kNsPerSec;
        std::vector<MidiPortRow> ports;
        for (const auto& p : adapters_.midi->listPorts()) {
            MidiPortRow r; r.identity = p.identity; r.name = p.name; r.isInput = p.isInput;
            std::lock_guard<std::mutex> g(sh.m);
            r.open = p.isInput ? sh.wantedInputs.count(p.identity) != 0 : sh.openOutputIdentity == p.identity;
            ports.push_back(std::move(r));
        }
        std::vector<LightingInterfaceRow> ifs;
        for (const auto& i : adapters_.packets->listInterfaces()) ifs.push_back({i.name, i.ipv4, i.up});
        std::lock_guard<std::mutex> g(sh.m);
        sh.midiPorts = std::move(ports);
        sh.ifaces = std::move(ifs);
        if (sh.learnPoll) {
            Shell* s = shell_.get();
            midi_.driver().post([s](ss::midi::MidiEngine& e) {
                std::lock_guard<std::mutex> g2(s->m);
                if (!s->learnPoll) return;
                if (e.learn().captured()) {
                    const auto* cap = e.learn().capture();
                    s->learn.active = true; s->learn.captured = true;
                    static const char* kType[] = {"Note", "CC", "Program change"};
                    s->learn.text = "Captured: channel " + std::to_string(cap->channel + 1) + ", " + kType[static_cast<int>(cap->type) % 3] + " " + std::to_string(cap->number);
                }
            });
        }
    }
    if (!sh.audioScanned && now - sh.startNs > 2 * ss::kNsPerSec) {
        sh.audioScanned = true;
        auto devs = adapters_.audio->listDevices();             // on demand only: it opens each device briefly
        std::vector<AudioDeviceRow> rows;
        for (const auto& d : devs) rows.push_back({d.typeName, d.deviceName, d.inputChannels, d.sampleRates});
        std::lock_guard<std::mutex> g(sh.m);
        sh.audioDevices = std::move(rows);
    }
    if (now >= sh.nextFree) {
        sh.nextFree = now + 5 * ss::kNsPerSec;
        std::string where = sh.dir;
        { std::lock_guard<std::mutex> g(sh.m); if (!sh.settings.recordingFolder.empty()) where = sh.settings.recordingFolder; }
        if (!where.empty()) {
            auto f = adapters_.files->freeBytes(where);
            std::lock_guard<std::mutex> g(sh.m);
            sh.freeValid = f.ok(); sh.freeBytes = f.ok() ? f.value() : 0;
        }
    }
}

// ============================================================================================================
// CONTROL thread: snapshot fields, staged state, perf adaptation
// ============================================================================================================
void AppController::shellFillUi(UiSnapshot& s, ss::Ns now) {
    auto& sh = *shell_;
    s.secondary = cfg_.secondary;
    s.recovery = recovery_.load();
    {
        std::lock_guard<std::mutex> g(sh.m);
        s.showFile.name = sh.showName;
        s.showFile.fileName = sh.sessionPath.empty() ? std::string() : ss::persist::baseName(sh.sessionPath);
        s.showFile.dirty = sh.dirty;
        s.showFile.autosavePending = sh.session && sh.session->autosavePending();
        s.showFile.autosaveNote = sh.autosaveNote;
        s.showFile.error = sh.fileError;
        s.showFile.scenes = sh.scenes;
        s.showFile.freeBytesValid = sh.freeValid; s.showFile.freeBytes = sh.freeBytes;
        s.recoveryInfo = sh.recoveryView;
        s.recoveryInfo.active = recovery_.load();
        s.broadcastEndpoint = sh.endpoint;
        s.audio.devices = sh.audioDevices;
        s.audio.currentType = sh.audioType; s.audio.currentName = sh.audioName;
        for (const auto& kv : sh.prefs.all()) s.prefs.emplace_back(kv.first, kv.second);
        s.midi.ports = sh.midiPorts;
        s.midi.learn = sh.learn;
        for (const auto& m : sh.mappings) {
            MidiMappingRow r;
            r.id = m.id; r.device = m.deviceName; r.channel = m.channel; r.type = static_cast<int>(m.type); r.number = m.number;
            r.valueMin = m.valueMin; r.valueMax = m.valueMax; r.encoder = static_cast<int>(m.encoder); r.actionId = m.actionId;
            if (const auto* info = ss::palette::Registry::find(static_cast<ss::ActionId>(m.actionId))) r.actionName = info->name;
            for (const auto& p : sh.midiPorts)
                if (p.isInput && (p.identity == m.deviceIdentity || (!m.deviceName.empty() && p.name == m.deviceName))) r.deviceAvailable = true;
            s.midi.mappings.push_back(std::move(r));
        }
        s.lighting.interfaceIp = sh.ifaceIp;
        s.lighting.destination = sh.lighting.destination;
        s.lighting.protocol = sh.lighting.protocol == ss::model::LightingProtocol::Sacn ? "sACN" : "Art-Net";
        s.lighting.port = sh.lighting.protocol == ss::model::LightingProtocol::Sacn ? 5568 : 6454;
        s.lighting.broadcast = sh.lighting.broadcast;
        s.lighting.universes = sh.universes; s.lighting.fixtures = sh.fixtures;
        s.lighting.patchIssues = sh.patchIssues; s.lighting.patchCanArm = sh.patchCanArm;
        s.lighting.interfaces = sh.ifaces;
        s.music = sh.music;
        s.perf.adaptStep = sh.adaptStep; s.perf.adaptMax = sh.adaptMax; s.perf.adaptNote = sh.adaptNote;
    }
    (void) now;
}

// FEAT-001..003 on the CONTROL thread, once per show tick (25 Hz, so the key colour updates well above 10 Hz).
void AppController::shellMusicTick(const ss::show::ShowSnapshot& snap, const ss::FeatureSnapshot& f, ss::Ns now) {
    auto& sh = *shell_;
    sh.beatGrid.anchorIfUnset(now);
    if (snap.tempo.valid()) sh.beatGrid.setTempo(snap.tempo.bpm, now, 0.05);
    // Phrase: one call per bar boundary of the show's grid. The tracker gates itself on tempo confidence.
    if (snap.gridValid && static_cast<std::int64_t>(snap.barIndex) != sh.lastBar) {
        sh.lastBar = static_cast<std::int64_t>(snap.barIndex);
        sh.phrase.onBar(sh.lastBar, static_cast<ss::audio::EnergyPhase>(f.arcPhase), snap.tempo.confidence);
    }
    sh.keyColor.update(snap.key, now);
    if (!sh.strobeSeen || sh.strobeLast != snap.strobingDisabled) {
        auto p = sh.beatLooks.params();
        p.strobingDisabled = snap.strobingDisabled;
        sh.beatLooks.setParams(p);
        sh.strobeSeen = true; sh.strobeLast = snap.strobingDisabled;
    }
    const auto ps = sh.phrase.state();
    const auto kc = sh.keyColor.color();
    const auto bs = sh.beatLooks.sample(sh.beatGrid, now);
    bool tint, beatOn;
    {
        std::lock_guard<std::mutex> g(sh.m);
        tint = sh.prefs.get("ui.keytint") == "1";
        beatOn = sh.prefs.get("lighting.beatlook") == "1";
        MusicView& m = sh.music;
        m.phraseLocked = ps.locked; m.phraseStart = ps.phraseStart; m.preDrop = ps.preDrop;
        m.barInPhrase = ps.barInPhrase; m.barsToPhraseStart = ps.barsToPhraseStart; m.phraseBars = sh.phrase.params().phraseBars;
        m.keyColorValid = snap.key.valid(); m.keyR = kc.r; m.keyG = kc.g; m.keyB = kc.b;
        m.beatValid = bs.valid; m.beatValue = bs.value; m.beatCycle = bs.cycle; m.beatCapped = bs.capped;
        m.keyTintOn = tint; m.beatLightOn = beatOn;
    }
    render_.setPaletteTint(tint && snap.key.valid(), kc.r, kc.g, kc.b);
    BeatLightCtx bc;
    bc.on = beatOn && !cfg_.secondary; bc.valid = bs.valid; bc.value = bs.value; bc.cycle = bs.cycle;
    bc.keyValid = snap.key.valid(); bc.r = kc.r; bc.g = kc.g; bc.b = kc.b;
    lighting_.setBeatLook(bc);
}

void AppController::controlAdapt(ss::Ns now) {
    auto& sh = *shell_;
    // Staged plugin state: swapped here, on the CONTROL thread, at a tick boundary.
    std::shared_ptr<StateBlobContent> staged;
    { std::lock_guard<std::mutex> g(sh.m); staged.swap(sh.staged); }
    if (staged) {
        lighting_.disarm(); midi_.disableOutput();             // restored state never arms anything (APP-SH-09)
        if (loadShow(staged->doc).ok()) {
            std::lock_guard<std::mutex> g(sh.m);
            for (const auto& kv : staged->prefs) sh.prefs.set(kv.first, kv.second);
            sh.savedHash.clear();
            sh.dirty = sh.session != nullptr;
            sh.fileError.clear();
        }
    }
    // Performance adaptation (1 Hz): the pressure comes from the render deadline counters and process CPU.
    if (now < sh.nextAdapt) return;
    sh.nextAdapt = now + ss::kNsPerSec;
    const RenderStats rs = render_.stats();
    ss::perf::PressureSample p;
    p.now = now;
    p.cpuAvailable = perf_.valid; p.cpuPct = perf_.cpuPct;
    const std::uint64_t dr = rs.framesRendered - sh.lastRendered, dd = rs.framesDropped - sh.lastDropped;
    sh.lastRendered = rs.framesRendered; sh.lastDropped = rs.framesDropped;
    p.deadlinesDue = dr + dd; p.deadlinesMissed = dd;
    const auto ev = sh.adapt.update(p);
    if (ev.kind != ss::perf::AdaptationEvent::Kind::None) diag_.post(DiagLevel::Info, "adapt", ev.detail);
    const auto st = sh.adapt.state();
    render_.setTargetFps(std::min<double>(cfg_.renderFps, static_cast<double>(st.sceneFps)));
    std::lock_guard<std::mutex> g(sh.m);
    sh.adaptStep = st.step; sh.adaptMax = sh.adapt.maxStep();
    sh.adaptNote = st.step == 0 ? std::string() : (sh.adapt.overloadWarningActive() ? "Overloaded. Quality is reduced." : "Quality is reduced to keep up.");
}

// Message thread, from UiModel::poll(): feeds the display router (the display provider is a UI-thread adapter).
void AppController::shellMessageTick() {
    auto& sh = *shell_;
    const ss::Ns now = clock().nowMonotonic();
    if (now < sh.nextTopology) return;
    sh.nextTopology = now + ss::kNsPerSec;
    auto topo = adapters_.displays->topology();
    std::lock_guard<std::mutex> g(sh.m);
    if (!sh.doc.outputs.empty()) {
        static std::uint64_t outputsVersion = 0;               // setOutputs resets assignments, so only on a changed show
        if (sh.router.routes().size() <= 2) { sh.router.setOutputs(sh.doc.outputs); ++outputsVersion; }
        const std::string roles = sh.prefs.get("display.roles");
        (void) roles;                                          // stored by Setup through SetPref; applied by the audience window code
    }
    sh.router.setTopology(topo);
}

// ============================================================================================================
// Plugin state blob
// ============================================================================================================
ss::Result<std::string> AppController::getStateBlob() const {
    StateBlobContent c;
    c.doc = const_cast<ss::show::ShowEngine&>(show_).document();       // document() is mutex-guarded; const_cast only for the API
    if (shell_) {
        std::lock_guard<std::mutex> g(shell_->m);
        for (const auto& kv : shell_->prefs.all()) c.prefs.emplace_back(kv.first, kv.second);
    }
    return encodeStateBlob(c);
}

ss::Status AppController::setStateBlob(const void* data, std::size_t size) {
    auto r = decodeStateBlob(data, size);                              // validated completely before anything changes
    if (!r.ok()) {
        if (shell_) { std::lock_guard<std::mutex> g(shell_->m); shell_->fileError = "The saved plugin state could not be used: " + r.error().detail; }
        return r.error();
    }
    if (r.value().referenceOnly) {
        const std::string msg = "This host kept only a reference to the show \"" + r.value().showName + "\". Open the .stagesignal file again.";
        { std::lock_guard<std::mutex> g(shell_->m); shell_->fileError = msg; }
        return ss::makeError(ss::ErrorCategory::UnsupportedCapability, msg);
    }
    auto content = std::make_shared<StateBlobContent>(std::move(r.value()));
    std::lock_guard<std::mutex> g(shell_->m);
    shell_->staged = std::move(content);
    return ss::okStatus();
}

}  // namespace ss::app
