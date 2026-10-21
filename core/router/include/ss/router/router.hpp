// StageSignal router (MOD-05 logic): logical outputs vs physical displays, roles, hotplug state machine,
// overlay policy and route generations (REQ-D01..D06, D08 flag, A09; IF-06). Pure logic: the app feeds in a
// topology snapshot whenever the OS reports a display change and applies the resulting routes to windows.
// Display *index* never appears here: a physical display is matched by its stable identity string.
#pragma once
#include <optional>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/base/time.hpp"
#include "ss/base/types.hpp"
#include "ss/model/show_model.hpp"

namespace ss::router {

constexpr OutputId kControlsOutputId = 1000;      // primary operator controls (never an audience output)
constexpr OutputId kMirrorOutputId = 1001;        // third screen as controls mirror
constexpr std::size_t kMaxDisplays = 16;
constexpr std::size_t kMaxLogicalOutputs = 16;

// ---- physical side ---------------------------------------------------------------------------
struct PhysicalDisplay {
    std::string identity;      // stable: EDID manufacturer/product/serial hash (or adapter-supplied equivalent)
    std::string connector;     // unique among the displays present right now (adapter path); NOT persistent identity
    int width = 0, height = 0; // current mode in pixels, as seen by the desktop (after rotation)
    int rotationDeg = 0;       // 0, 90, 180, 270
    double dpiScale = 1.0;
    bool primary = false;      // the OS primary display
};
using Topology = std::vector<PhysicalDisplay>;

// ---- logical side ----------------------------------------------------------------------------
enum class RouteStatus : std::uint8_t {
    Unassigned,          // no display chosen
    Active,              // bound to a present display and showing
    Disconnected,        // assigned display is not present; nothing is drawn anywhere else
    NeedsConfirmation,   // identity ambiguous or restore unsafe; idle until the operator confirms
    Suspended,           // controls fell back onto this route's display; route is idle and blacked out
    Disabled             // measured capability does not allow this extra output
};
const char* routeStatusName(RouteStatus s);

struct OverlayPolicy {
    bool showBpmKey = false, showText = true, showBranding = false;
    bool showDiagnostics = false;          // audience/visual outputs: only when explicitly enabled
};

struct RouteInfo {
    OutputRoute route;                     // IF-06 value for the renderer (connected == status Active)
    std::string label;
    bool isControls = false, isMirror = false;
    RouteStatus status = RouteStatus::Unassigned;
    std::string statusText;                // plain English, sanitized
    std::string boundConnector;            // transient OS connector currently used ("" when not bound)
    std::string suggestedConnector;        // last confirmed connector, offered as a one-click confirm
    bool finalBlackout = false;            // final composition forced black on this route (never set on controls)
    bool idle = true;                      // true unless Active
    bool ownsShowState = false;            // always false: the show module is the single writer
};

struct AssignmentRecord {                  // persisted per machine, not in the portable show export
    OutputId output = 0;
    std::string identity;
    std::string connectorHint;
};
struct RouterConfig { std::vector<AssignmentRecord> assignments; };

struct CapabilityLimits {
    int maxVisualOutputs = 8;              // total active audience/visual routes the machine may drive
    int maxIndependentOutputs = 8;         // of which independent compositions
    std::string reason;                    // e.g. "BEN-02 measured 2 outputs at 25 FPS"; shown to the operator
};

struct RouterEvent {
    enum class Type : std::uint8_t {
        DisplayAdded, DisplayRemoved, Disconnected, Reattached, NeedsConfirmation, Confirmed, ViewportChanged,
        ControlsFallback, ControlsRestored, Suspended, Resumed, Disabled, Enabled
    } type = Type::DisplayAdded;
    OutputId output = 0;
    std::string text;                      // journal-ready ("device" kind), no identities or paths
};

struct IdentifyLabel {
    std::string connector;                 // which physical display to draw on (transient key)
    int ordinal = 0;                       // 1..N, stable ordering by connector
    std::string title;                     // "Display 2"
    std::string subtitle;                  // "Audience: Main  1920x1080" / "Controls" / "Not assigned"
    bool borderGrid = true;                // projector calibration border/grid
};
struct IdentifySession {
    bool active = false;
    Ns startedAt = 0, expiresAt = 0;
    std::vector<IdentifyLabel> labels;
    bool activeAt(Ns now) const { return active && now < expiresAt; }
};

class DisplayRouter {
public:
    DisplayRouter();

    // ---- configuration ----
    // Logical visual outputs from the show document. Controls/mirror outputs are added by the router.
    // Assignments of ids that persist are kept. Validates ids and duplicate sources.
    Status setOutputs(const std::vector<model::OutputDef>& outputs);
    RouterConfig config() const;
    Status applyConfig(const RouterConfig& cfg);       // restores roles by identity; unknown outputs are ignored
    void setCapability(const CapabilityLimits& limits);

    // ---- topology (startup, hotplug, rotation/resolution/DPI change) ----
    Status setTopology(const Topology& t, std::vector<RouterEvent>* events = nullptr);

    // ---- operator commands ----
    Status assignControls(const std::string& identity, const std::string& connector);
    Status assignMirror(const std::string& identity, const std::string& connector);   // third screen as controls
    Status assignOutput(OutputId id, const std::string& identity, const std::string& connector);
    Status unassign(OutputId id);
    // Resolve NeedsConfirmation by choosing a present display (by transient connector).
    Status confirm(OutputId id, const std::string& connector);
    Status setOverlay(OutputId id, const OverlayPolicy& p);        // kAllOutputs applies to all visual outputs
    Status setBpmKeyVisible(OutputId id, bool on);                  // ToggleOverlayBpmKey backing
    Status setDiagnostics(OutputId id, bool on);                    // explicit opt-in for visual outputs
    // Final visual blackout flag per route (REQ-D08). Controls are never blacked out.
    void setFinalBlackout(OutputId target, bool on);

    // ---- queries ----
    std::vector<RouteInfo> routes() const;                          // controls, mirror, then visual outputs by id
    std::optional<RouteInfo> find(OutputId id) const;
    std::vector<PhysicalDisplay> unassignedDisplays() const;
    // Display the operator controls currently appear on (assigned, or the fallback when it is gone).
    bool controlsDisplay(PhysicalDisplay* out) const;
    bool controlsOnFallback() const { return controlsFallback_; }
    // Plain-English explanation for why a visual output is not showing / is disabled (REQ-D03).
    std::string explain(OutputId id) const;
    std::uint64_t tableVersion() const { return version_; }         // bumps on any change a window must apply
    bool globalBlackout() const { return blackoutAll_; }
    // Operator-triggered only: nothing is drawn unless this is called (IdentifyDisplays action).
    IdentifySession identify(Ns now, Ns duration = 10 * kNsPerSec) const;
    // Self-check used by tests and by the Show Doctor: empty means every safety invariant holds.
    std::vector<std::string> invariantViolations() const;
    const Topology& topology() const { return topo_; }

private:
    struct Out {
        OutputId id = 0;
        std::string label;
        RouteKind kind = RouteKind::Audience;
        bool controls = false, mirror = false;
        OutputId duplicateOf = 0;
        OverlayPolicy overlay;
        std::string identity, connectorHint;            // assignment
        RouteStatus status = RouteStatus::Unassigned;
        std::string statusText, bound;
        std::string pendingBind, suggestedHint;         // one-shot operator choice; offered confirm suggestion
        bool blackout = false;
        Generation generation = 0;
        int vw = 800, vh = 600, rot = 0;
        std::string reason;                             // pending-confirmation / disabled reason
    };
    Out* mut(OutputId id);
    const Out* get(OutputId id) const;
    const PhysicalDisplay* byConnector(const std::string& c) const;
    std::vector<const PhysicalDisplay*> matches(const std::string& identity) const;
    Status assignCommon(OutputId id, const std::string& identity, const std::string& connector);
    void reconcile(std::vector<RouterEvent>* ev);
    void applyCapability(std::vector<RouterEvent>* ev);
    void setStatus(Out& o, RouteStatus s, const std::string& text, std::vector<RouterEvent>* ev, RouterEvent::Type t);
    RouteInfo toInfo(const Out& o) const;
    std::string fingerprint() const;

    std::vector<Out> outs_;      // controls, mirror, then visual outputs ascending by id
    Topology topo_;
    CapabilityLimits cap_;
    bool blackoutAll_ = false, controlsFallback_ = false;
    std::string controlsBound_;  // connector currently hosting controls
    std::uint64_t version_ = 1;
};

// Aspect classification of a viewport after rotation (REQ-D07 helper: the renderer recomputes its normalised
// composition from this; show content is never changed by a rotation or resolution change).
struct ViewportClass {
    enum class Kind : std::uint8_t { Ratio4x3, Ratio16x9, Ratio16x10, Portrait, Other } kind = Kind::Other;
    double aspect = 1.0;     // width / height
};
ViewportClass classifyViewport(int width, int height);

// Names of the overlay layers an audience capture must contain for this route (REQ-A09 test helper).
std::vector<std::string> enabledOverlayLayers(const OutputRoute& r);

}  // namespace ss::router
