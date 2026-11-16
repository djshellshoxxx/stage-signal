// Private state of AppController's shell services (see AppShell.cpp). Included by AppController.cpp and AppShell.cpp only.
#pragma once
#include <climits>
#include <mutex>
#include <set>
#include "App/AppController.h"
#include "App/ShellState.h"
#include "ss/audio/phrase_tracker.hpp"
#include "ss/palette/key_color.hpp"
#include "ss/show/beat_looks.hpp"
#include "ss/palette/keymap.hpp"
#include "ss/perf/adaptation.hpp"
#include "ss/persist/session.hpp"
#include "ss/persist/settings.hpp"
#include "ss/router/router.hpp"
#include "ss/diag/sampler.hpp"

namespace ss::app {

struct AppController::Shell {
    mutable std::mutex m;
    std::atomic<bool> ready{false};            // IO thread waits for settings before touching the rest
    // persistence
    std::string dir, settingsPath, prefsPath, lockPath, untitledPath;
    ss::persist::LocalSettings settings;
    PrefsStore prefs;
    bool ownsLock = false;
    std::uint64_t benchmarkSavedGeneration = 0;
    // show file
    std::unique_ptr<ss::persist::ShowSession> session;
    std::string sessionPath;
    std::string savedHash;                     // hash of the document at load/save
    std::uint64_t lastCheckedRev = 0;
    ss::Ns nextDirtyCheck = 0, nextFree = 0, nextPorts = 0;
    bool dirty = false;
    bool sawAutosavePending = false;
    std::string fileError, autosaveNote;
    std::vector<ScenePadInfo> scenes;
    std::string showName;
    bool freeValid = false;
    std::uint64_t freeBytes = 0;
    // recovery
    bool haveProposal = false;
    ss::persist::RestoreProposal proposal;
    RecoveryView recoveryView;
    // device caches
    std::vector<AudioDeviceRow> audioDevices;
    bool audioScanned = false;
    ss::Ns audioScanAt = 0;
    std::vector<MidiPortRow> midiPorts;
    std::vector<LightingInterfaceRow> ifaces;
    std::string audioType, audioName, ifaceIp;
    std::set<std::string> wantedInputs;
    std::string openOutputIdentity;
    IMidiOutPort* outPort = nullptr;
    // MIDI
    std::vector<ss::model::MidiMapping> mappings;
    MidiLearnView learn;
    bool learnPoll = false;
    // lighting
    ss::model::ShowDocument doc;               // last loaded document (profiles, patches, lighting settings)
    ss::model::LightingSettings lighting;
    std::vector<std::string> patchIssues;
    bool patchCanArm = true;
    int universes = 0, fixtures = 0;
    // broadcast endpoint (non-secret part)
    BroadcastEndpointView endpoint;
    // staged plugin state
    std::shared_ptr<StateBlobContent> staged;
    // cores
    ss::palette::KeyMap keymap = ss::palette::KeyMap::defaults();
    ss::router::DisplayRouter router;
    ss::Ns nextTopology = 0;
    ss::perf::AdaptationController adapt;
    ss::Ns nextAdapt = 0;
    std::uint64_t lastRendered = 0, lastDropped = 0;
    int adaptStep = 0, adaptMax = 0;
    std::string adaptNote;
    ss::diag::FeatureHistory history;
    ss::Ns startNs = 0;
    std::int64_t startWallMs = 0;
    std::string lastRouteNote;
    // musical features (CONTROL thread, published through `music`)
    ss::audio::PhraseTracker phrase;
    ss::palette::KeyColorMapper keyColor;
    ss::show::BeatLooks beatLooks;
    ss::show::BeatGrid beatGrid;
    std::int64_t lastBar = INT64_MIN;
    bool strobeSeen = false, strobeLast = false;
    MusicView music;
    bool keyTintOn = false, beatLightOn = false;
};

}  // namespace ss::app
