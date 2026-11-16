#include "ss/palette/registry.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace ss::palette {

namespace {

constexpr ValueSpec kNone{};
constexpr ValueSpec kNorm{true, ValueKind::Normalized, "%", 0.0, 1.0, false, 0.5, 0};
constexpr ValueSpec kAmount{true, ValueKind::Normalized, "%", 0.0, 1.0, false, 0.0, 0};
constexpr ValueSpec kSlot{true, ValueKind::SceneSlot, "pad", 0.0, 7.0, true, 0.0, 1};
constexpr ValueSpec kMode{true, ValueKind::Mode, "mode", 0.0, 2.0, true, 0.0, 0};
constexpr ValueSpec kBpm{true, ValueKind::Bpm, "BPM", 20.0, 300.0, false, 120.0, 0};
constexpr ValueSpec kCue{true, ValueKind::Index, "cue", 0.0, 9999.0, true, 0.0, 0};
constexpr ValueSpec kText{true, ValueKind::Index, "message id", 0.0, 4294967295.0, true, 0.0, 0};
constexpr ValueSpec kKey{true, ValueKind::KeyIndex, "key", 0.0, 23.0, true, 0.0, 0};
constexpr ValueSpec kOut{true, ValueKind::Index, "output id", 1.0, 4294967294.0, true, 1.0, 0};

#define A(id, name, cat, desc, val, shortcut, kw) \
    ActionInfo{ActionId::id, #id, name, Category::cat, desc, val, shortcut, kw, false}

std::vector<ActionInfo> build() {
    std::vector<ActionInfo> v = {
        A(SceneSelect, "Select scene pad", Scenes, "Jump to scene pad 1 to 8 on the current bank.", kSlot, "1-8", "pad jump scene"),
        A(NextScene, "Next scene", Scenes, "Advance to the next scene.", kNone, "F5", "forward advance"),
        A(PrevScene, "Previous scene", Scenes, "Go back to the previous scene.", kNone, "", "back"),
        A(BankNext, "Next scene bank", Scenes, "Show the next bank of 8 scene pads.", kNone, "", "page"),
        A(BankPrev, "Previous scene bank", Scenes, "Show the previous bank of 8 scene pads.", kNone, "", "page"),
        A(Pause, "Pause automation", Scenes, "Hold the current scene; automatic changes stop until resumed.", kNone, "F6", "hold freeze"),
        A(Resume, "Resume automation", Scenes, "Let automatic scene changes continue.", kNone, "F7", "continue play"),
        A(SetMode, "Set show mode", Scenes, "Choose Auto, Programmed or Hybrid scene selection.", kMode, "", "auto programmed hybrid"),
        A(NextCue, "Next cue", Scenes, "Advance the programmed cue list by one cue.", kNone, "", "go"),
        A(PrevCue, "Previous cue", Scenes, "Step the programmed cue list back by one cue.", kNone, "", "back"),
        A(GoToCue, "Go to cue", Scenes, "Jump to a cue by its index.", kCue, "", "jump"),

        A(VisualBlackout, "Visual blackout", Blackout, "Black out visual outputs only. Latched until restored.", kNone, "", "dark black screen"),
        A(LightingBlackout, "Lighting blackout", Blackout, "Black out lighting fixtures only. Latched until restored.", kNone, "", "dark dmx"),
        A(EmergencyBlackout, "Emergency blackout", Blackout, "Combined visual and lighting blackout, latched. Recording is not affected.", kNone, "F8", "panic dark kill all"),
        A(RestoreBlackout, "Restore from blackout", Blackout, "Explicitly restore the state held before the blackout latch.", kNone, "Shift+F8", "undo unblack"),

        A(Brightness, "Brightness", Look, "Overall brightness of the active scene.", kNorm, "", "dim light level"),
        A(Speed, "Speed", Look, "Animation speed of the active scene.", kNorm, "", "tempo rate motion"),
        A(Intensity, "Intensity", Look, "Reactive intensity of the active scene.", kNorm, "", "reactivity amount"),
        A(ColorHue, "Colour hue", Look, "Hue of the scene palette.", kNorm, "", "color"),
        A(ColorSaturation, "Colour saturation", Look, "Saturation of the scene palette.", kNorm, "", "color vividness"),

        A(SetBpm, "Set BPM", Tempo, "Override the detected tempo with a manual value.", kBpm, "", "tempo manual override"),
        A(TapTempo, "Tap tempo", Tempo, "Tap to set the tempo by hand.", kNone, "T", "tap beat"),
        A(ReleaseBpmOverride, "Release BPM override", Tempo, "Return to the detected tempo.", kNone, "", "auto tempo"),
        A(SetKey, "Set musical key", Tempo, "Override the detected key (0 to 11 major C to B, 12 to 23 minor C to B).", kKey, "", "manual override"),
        A(ReleaseKeyOverride, "Release key override", Tempo, "Return to the detected key.", kNone, "", "auto key"),

        A(TextPublish, "Publish text", Text, "Publish the previewed text message to the selected audience outputs.", kNone, "", "message show"),
        A(TextClear, "Clear text", Text, "Remove published text from the audience outputs.", kNone, "", "message hide"),
        A(TextSelect, "Select text message", Text, "Choose a text message as the draft by id.", kText, "", "message draft"),

        A(DisableStrobing, "Disable strobing", Safety, "Suppress flashing and strobe content everywhere.", kNone, "", "flash photosensitive epilepsy"),
        A(EnableStrobing, "Enable strobing", Safety, "Allow scenes that contain flashing content.", kNone, "", "flash"),

        A(ArmLighting, "Arm lighting output", Lighting, "Allow lighting data to be transmitted to the network.", kNone, "", "dmx artnet sacn enable"),
        A(DisarmLighting, "Disarm lighting output", Lighting, "Stop transmitting lighting data.", kNone, "", "dmx artnet sacn disable"),
        A(LightingDryRun, "Lighting dry run", Lighting, "Compute lighting frames without transmitting them.", kNone, "", "dmx test preview"),

        A(StartRecording, "Start recording", Recording, "Begin recording to the selected folder.", kNone, "", "record rec"),
        A(StopRecording, "Stop recording", Recording, "Stop and finalise the recording.", kNone, "", "record rec"),
        A(AddMarker, "Add marker", Recording, "Drop a timestamped marker in the journal and recording.", kNone, "", "bookmark note"),
        A(StartBroadcast, "Start broadcast", Recording, "Begin streaming to the configured destination.", kNone, "", "stream live"),
        A(StopBroadcast, "Stop broadcast", Recording, "End the live stream.", kNone, "", "stream live"),

        A(EffectPrismMirror, "Prism Mirror amount", Effects, "Amount of the Prism Mirror effect; 0 turns it off.", kAmount, "", "kaleidoscope"),
        A(EffectBloomPulse, "Bloom Pulse amount", Effects, "Amount of the Bloom Pulse effect; 0 turns it off.", kAmount, "", "glow"),

        A(Undo, "Undo", Edit, "Undo the last show change.", kNone, "Ctrl+Z", "revert"),
        A(Redo, "Redo", Edit, "Redo the change that was undone.", kNone, "Ctrl+Y", "repeat"),
        A(SnapshotStoreA, "Store snapshot A", Edit, "Save the current look into snapshot A.", kNone, "", "ab compare"),
        A(SnapshotStoreB, "Store snapshot B", Edit, "Save the current look into snapshot B.", kNone, "", "ab compare"),
        A(SnapshotRecallA, "Recall snapshot A", Edit, "Switch to the look saved in snapshot A.", kNone, "", "ab compare"),
        A(SnapshotRecallB, "Recall snapshot B", Edit, "Switch to the look saved in snapshot B.", kNone, "", "ab compare"),
        A(OpenCommandPalette, "Open command palette", Edit, "Search every action by name.", kNone, "Ctrl+K", "search find"),
        A(RunShowDoctor, "Run Show Doctor", Edit, "Check devices, media and settings before the show.", kNone, "", "preflight check"),
        A(ExportShowReport, "Export show report", Edit, "Write the setlist, energy and tempo report.", kNone, "", "setlist csv html"),

        A(SelectOutput, "Select target output", Output, "Choose which audience output the controls address.", kOut, "", "screen display target"),
        A(ToggleOverlayBpmKey, "Toggle BPM and key overlay", Output, "Show or hide tempo and key on the selected output.", kNone, "", "overlay"),
        A(IdentifyDisplays, "Identify displays", Output, "Show a label on every display for a few seconds.", kNone, "", "monitor screen"),

        A(SaveShow, "Save show", File, "Save the current show file.", kNone, "Ctrl+S", "write"),
        A(OpenShow, "Open show", File, "Open a show file.", kNone, "", "load"),
        A(ExportShow, "Export portable show", File, "Export the show with its media as a portable bundle.", kNone, "", "bundle package"),
    };
#undef A
    for (auto& i : v) i.safetyCritical = isSafetyCritical(i.id);
    std::sort(v.begin(), v.end(), [](const ActionInfo& a, const ActionInfo& b) { return id(a.id) < id(b.id); });
    return v;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

}  // namespace

const char* categoryName(Category c) {
    switch (c) {
        case Category::Scenes: return "Scenes";
        case Category::Blackout: return "Blackout";
        case Category::Look: return "Look";
        case Category::Tempo: return "Tempo and key";
        case Category::Text: return "Text";
        case Category::Safety: return "Safety";
        case Category::Lighting: return "Lighting";
        case Category::Recording: return "Recording and broadcast";
        case Category::Effects: return "Effects";
        case Category::Edit: return "Edit and tools";
        case Category::Output: return "Outputs";
        case Category::File: return "Show file";
    }
    return "";
}

bool parseCategory(const std::string& name, Category& out) {
    const std::string n = lower(trim(name));
    for (int i = 0; i < kCategoryCount; ++i) {
        const Category c = static_cast<Category>(i);
        const std::string full = lower(categoryName(c));
        if (n == full || (!n.empty() && full.compare(0, n.size(), n) == 0)) { out = c; return true; }
    }
    return false;
}

const std::vector<ActionInfo>& Registry::all() {
    static const std::vector<ActionInfo> v = build();
    return v;
}

const ActionInfo* Registry::find(ActionId a) {
    const auto& v = all();
    auto it = std::lower_bound(v.begin(), v.end(), id(a), [](const ActionInfo& i, std::uint32_t x) { return id(i.id) < x; });
    return (it != v.end() && it->id == a) ? &*it : nullptr;
}

const ActionInfo* Registry::findByIdent(const std::string& ident) {
    for (const auto& i : all()) if (ident == i.ident) return &i;
    return nullptr;
}

Result<double> parseValue(const ActionInfo& info, const std::string& textIn) {
    auto bad = [](const char* why) { return Result<double>(makeError(ErrorCategory::InvalidArgument, why)); };
    if (!info.value.takesValue) return 0.0;
    std::string t = lower(trim(textIn));
    if (t.empty()) return info.value.defaultValue;
    const ValueSpec& v = info.value;
    if (v.kind == ValueKind::Mode) {
        if (t == "auto") return 0.0;
        if (t == "programmed" || t == "programmed ") return 1.0;
        if (t == "hybrid") return 2.0;
    }
    bool percent = false;
    if (!t.empty() && t.back() == '%') { percent = true; t.pop_back(); t = trim(t); }
    if (percent && v.kind != ValueKind::Normalized) return bad("percent is only valid for 0..1 values");
    if (t.empty()) return bad("missing number");
    char* end = nullptr;
    const double raw = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(raw)) return bad("not a number");
    double x = raw;
    if (v.kind == ValueKind::Normalized && percent) x = raw / 100.0;
    else x = raw - v.uiOffset;                       // scene pads are typed 1..8 and stored 0..7
    if (v.integer && std::fabs(x - std::round(x)) > 1e-9) return bad("whole number required");
    if (v.integer) x = std::round(x);
    if (x < v.min - 1e-12 || x > v.max + 1e-12) return bad("value out of range");
    return x;
}

Result<Action> makeAction(ActionId aid, const std::string& valueText, ActionSource source) {
    const ActionInfo* info = Registry::find(aid);
    if (!info) return makeError(ErrorCategory::InvalidArgument, "unknown action");
    Action a;
    a.actionId = id(aid);
    a.source = source;
    if (info->value.takesValue) {
        auto v = parseValue(*info, valueText);
        if (!v.ok()) return v.error();
        a.value = v.value();
    }
    return a;
}

}  // namespace ss::palette
