#include "ss/router/router.hpp"
#include <algorithm>
#include <set>

namespace ss::router {

namespace {
using RT = RouterEvent::Type;

Status invalid(const std::string& m) { return Status(makeError(ErrorCategory::InvalidArgument, m)); }
std::string clip(const std::string& s, std::size_t n) { return s.size() > n ? s.substr(0, n) : s; }
std::string mode(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }
bool validRotation(int r) { return r == 0 || r == 90 || r == 180 || r == 270; }
}  // namespace

const char* routeStatusName(RouteStatus s) {
    switch (s) {
        case RouteStatus::Unassigned: return "unassigned";
        case RouteStatus::Active: return "active";
        case RouteStatus::Disconnected: return "disconnected";
        case RouteStatus::NeedsConfirmation: return "needs-confirmation";
        case RouteStatus::Suspended: return "suspended";
        case RouteStatus::Disabled: return "disabled";
    }
    return "unassigned";
}

std::vector<std::string> enabledOverlayLayers(const OutputRoute& r) {
    std::vector<std::string> v;
    if (r.showText) v.push_back("text");
    if (r.showBpmKey) v.push_back("bpm-key");
    if (r.showBranding) v.push_back("branding");
    if (r.showDiagnostics) v.push_back("diagnostics");
    return v;
}

DisplayRouter::DisplayRouter() {
    Out c; c.id = kControlsOutputId; c.label = "Controls"; c.kind = RouteKind::Controls; c.controls = true; c.generation = 1;
    Out m; m.id = kMirrorOutputId; m.label = "Controls mirror"; m.kind = RouteKind::Controls; m.controls = true; m.mirror = true; m.generation = 1;
    c.statusText = m.statusText = "Not assigned";
    outs_ = {c, m};
}

DisplayRouter::Out* DisplayRouter::mut(OutputId id) {
    for (Out& o : outs_) if (o.id == id) return &o;
    return nullptr;
}
const DisplayRouter::Out* DisplayRouter::get(OutputId id) const {
    for (const Out& o : outs_) if (o.id == id) return &o;
    return nullptr;
}
const PhysicalDisplay* DisplayRouter::byConnector(const std::string& c) const {
    for (const PhysicalDisplay& d : topo_) if (d.connector == c) return &d;
    return nullptr;
}
std::vector<const PhysicalDisplay*> DisplayRouter::matches(const std::string& identity) const {
    std::vector<const PhysicalDisplay*> v;
    if (identity.empty()) return v;
    for (const PhysicalDisplay& d : topo_) if (d.identity == identity) v.push_back(&d);
    return v;
}

// ---- configuration -------------------------------------------------------------------------
Status DisplayRouter::setOutputs(const std::vector<model::OutputDef>& defs) {
    if (defs.size() > kMaxLogicalOutputs) return invalid("too many logical outputs");
    std::set<OutputId> ids;
    for (const model::OutputDef& d : defs) {
        if (d.id == 0 || d.id == kAllOutputs || d.id >= kControlsOutputId) return invalid("logical output id out of range");
        if (!ids.insert(d.id).second) return invalid("duplicate logical output id");
        if (d.kind == RouteKind::Controls) return invalid("controls are managed by the router, not the show outputs");
    }
    for (const model::OutputDef& d : defs) {
        if (d.kind != RouteKind::Duplicate) continue;
        const model::OutputDef* src = nullptr;
        for (const auto& s : defs) if (s.id == d.duplicateOf) src = &s;
        if (!src || src->id == d.id || src->kind == RouteKind::Duplicate)
            return invalid("duplicate output must reference another non-duplicate output");
    }
    std::vector<Out> next(outs_.begin(), outs_.begin() + 2);
    std::vector<model::OutputDef> sorted = defs;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const model::OutputDef& d : sorted) {
        Out o;
        if (const Out* old = get(d.id)) o = *old;
        o.id = d.id; o.label = clip(d.label, 64); o.kind = d.kind; o.duplicateOf = d.kind == RouteKind::Duplicate ? d.duplicateOf : 0;
        o.overlay.showBpmKey = d.showBpmKey; o.overlay.showText = d.showText; o.overlay.showBranding = d.showBranding;
        if (!get(d.id)) { o.overlay.showDiagnostics = false; o.generation = 1; o.statusText = "Not assigned"; }
        o.controls = false; o.mirror = false;
        next.push_back(o);
    }
    outs_ = next;
    reconcile(nullptr);
    return okStatus();
}

RouterConfig DisplayRouter::config() const {
    RouterConfig c;
    for (const Out& o : outs_) if (!o.identity.empty()) c.assignments.push_back({o.id, o.identity, o.connectorHint});
    return c;
}

Status DisplayRouter::applyConfig(const RouterConfig& cfg) {
    for (const AssignmentRecord& a : cfg.assignments)
        if (a.identity.size() > 256 || a.connectorHint.size() > 256) return invalid("assignment record too long");
    for (const AssignmentRecord& a : cfg.assignments) {
        Out* o = mut(a.output);
        if (!o) continue;                       // output no longer exists in this show
        o->identity = a.identity; o->connectorHint = a.connectorHint;
    }
    reconcile(nullptr);
    return okStatus();
}

void DisplayRouter::setCapability(const CapabilityLimits& limits) {
    cap_ = limits;
    cap_.maxVisualOutputs = std::max(0, limits.maxVisualOutputs);
    cap_.maxIndependentOutputs = std::max(0, limits.maxIndependentOutputs);
    reconcile(nullptr);
}

// ---- topology ------------------------------------------------------------------------------
Status DisplayRouter::setTopology(const Topology& t, std::vector<RouterEvent>* events) {
    if (t.size() > kMaxDisplays) return invalid("too many displays");
    std::set<std::string> conns;
    for (const PhysicalDisplay& d : t) {
        if (d.connector.empty() || !conns.insert(d.connector).second) return invalid("display connectors must be unique and non-empty");
        if (d.identity.empty()) return invalid("display identity missing");
        if (d.identity.size() > 256 || d.connector.size() > 256) return invalid("display strings too long");
        if (d.width <= 0 || d.height <= 0 || d.width > 16384 || d.height > 16384) return invalid("display size out of range");
        if (!validRotation(d.rotationDeg)) return invalid("display rotation must be 0, 90, 180 or 270");
    }
    if (events) {
        for (const PhysicalDisplay& d : t) {
            bool had = false;
            for (const auto& p : topo_) if (p.connector == d.connector && p.identity == d.identity) had = true;
            if (!had) events->push_back({RT::DisplayAdded, 0, "A display was connected"});
        }
        for (const auto& p : topo_) {
            bool still = false;
            for (const auto& d : t) if (p.connector == d.connector && p.identity == d.identity) still = true;
            if (!still) events->push_back({RT::DisplayRemoved, 0, "A display was disconnected"});
        }
    }
    topo_ = t;
    reconcile(events);
    return okStatus();
}

std::string DisplayRouter::fingerprint() const {
        std::string f = std::to_string(blackoutAll_) + controlsBound_;
        for (const Out& o : outs_) {
            f += "|" + std::to_string(o.id) + ":" + std::to_string(static_cast<int>(o.status)) + ":" + o.bound + ":" +
                 std::to_string(o.generation) + ":" + std::to_string(o.vw) + "x" + std::to_string(o.vh) + "r" + std::to_string(o.rot) +
                 ":" + (o.blackout ? "b" : "-") + (o.overlay.showBpmKey ? "k" : "-") + (o.overlay.showText ? "t" : "-") +
                 (o.overlay.showBranding ? "r" : "-") + (o.overlay.showDiagnostics ? "d" : "-") + ":" + o.identity;
        }
        return f;
}

void DisplayRouter::setStatus(Out& o, RouteStatus s, const std::string& text, std::vector<RouterEvent>* ev, RT t) {
    const bool changed = o.status != s || o.statusText != text;
    if (changed && ev && (o.status != s)) ev->push_back({t, o.id, o.label + ": " + text});
    o.status = s;
    o.statusText = text;
}

void DisplayRouter::reconcile(std::vector<RouterEvent>* ev) {
    const std::string before = fingerprint();

    std::set<std::string> occupied;
    auto applyViewport = [&](Out& o, const PhysicalDisplay& d) {
        if (o.vw != d.width || o.vh != d.height || o.rot != d.rotationDeg) {
            const bool first = o.bound.empty();
            o.vw = d.width; o.vh = d.height; o.rot = d.rotationDeg;
            if (!first) {
                ++o.generation;
                if (ev) ev->push_back({RT::ViewportChanged, o.id, o.label + ": display mode changed to " + mode(d.width, d.height)});
            }
        }
    };
    // Resolve an output's assignment against the present displays. Returns the connector to bind, "" if missing,
    // or sets `ambiguous` when several identical displays match and nothing disambiguates.
    auto resolve = [&](Out& o, bool* ambiguous) -> std::string {
        *ambiguous = false;
        if (!o.pendingBind.empty()) {   // operator just chose this exact display
            const PhysicalDisplay* d = byConnector(o.pendingBind);
            const std::string c = d && d->identity == o.identity ? d->connector : std::string();
            o.pendingBind.clear();
            if (!c.empty()) return c;
        }
        if (!o.bound.empty() && (o.status == RouteStatus::Active || o.status == RouteStatus::Suspended)) {
            const PhysicalDisplay* d = byConnector(o.bound);
            if (d && d->identity == o.identity) return o.bound;     // already running here; nothing changed for this unit
        }
        const auto m = matches(o.identity);
        if (m.empty()) return "";
        if (m.size() == 1) return m[0]->connector;
        *ambiguous = true;
        return "";
    };

    // ---- controls (primary) ----
    {
        Out& c = outs_[0];
        std::string conn;
        bool amb = false;
        if (c.identity.empty()) {
            // default: the OS primary display, else the first one
            const PhysicalDisplay* pick = nullptr;
            for (const auto& d : topo_) if (d.primary) pick = &d;
            if (!pick && !topo_.empty()) pick = &*std::min_element(topo_.begin(), topo_.end(), [](const auto& a, const auto& b) { return a.connector < b.connector; });
            conn = pick ? pick->connector : "";
            controlsFallback_ = false;
            if (pick) { applyViewport(c, *pick); c.bound = conn; setStatus(c, RouteStatus::Active, "Controls on the primary display", ev, RT::Reattached); }
            else { c.bound.clear(); setStatus(c, RouteStatus::Disconnected, "No display is connected", ev, RT::Disconnected); }
        } else {
            conn = resolve(c, &amb);
            if (!conn.empty()) {
                const PhysicalDisplay* d = byConnector(conn);
                const bool wasFallback = controlsFallback_;
                controlsFallback_ = false;
                applyViewport(c, *d);
                c.bound = conn;
                const bool wasActive = c.status == RouteStatus::Active;
                setStatus(c, RouteStatus::Active, "Controls on their assigned display", ev, RT::ControlsRestored);
                if (wasFallback && wasActive && ev) ev->push_back({RT::ControlsRestored, c.id, "Controls returned to their assigned display"});
            } else {
                // Assigned display is gone (or ambiguous): controls must stay reachable, so fall back to a display
                // that is not already running an active output if possible, else the primary.
                const bool wasFallback = controlsFallback_;
                const PhysicalDisplay* pick = nullptr;
                for (const auto& d : topo_) if (d.primary) pick = &d;
                if (!pick) {
                    for (const auto& d : topo_) {
                        bool used = false;
                        for (std::size_t i = 1; i < outs_.size(); ++i) if (outs_[i].status == RouteStatus::Active && outs_[i].bound == d.connector) used = true;
                        if (!used && (!pick || d.connector < pick->connector)) pick = &d;
                    }
                }
                if (!pick && !topo_.empty()) pick = &topo_.front();
                if (pick) {
                    applyViewport(c, *pick);
                    c.bound = pick->connector;
                    controlsFallback_ = true;
                    setStatus(c, RouteStatus::Active, amb ? "Assigned controls display is ambiguous; controls are on a fallback display"
                                                          : "Assigned controls display is missing; controls are on a fallback display",
                              ev, RT::ControlsFallback);
                    if (!wasFallback && ev) ev->push_back({RT::ControlsFallback, c.id, "Controls moved to a fallback display"});
                } else {
                    c.bound.clear();
                    controlsFallback_ = false;
                    setStatus(c, RouteStatus::Disconnected, "No display is connected", ev, RT::Disconnected);
                }
            }
        }
        controlsBound_ = c.bound;
        if (!c.bound.empty()) occupied.insert(c.bound);
    }

    // ---- mirror (third screen as controls) ----
    {
        Out& m = outs_[1];
        if (m.identity.empty()) {
            m.bound.clear();
            setStatus(m, RouteStatus::Unassigned, "Not assigned", ev, RT::Disconnected);
        } else {
            bool amb = false;
            const std::string conn = resolve(m, &amb);
            if (conn.empty()) {
                m.bound.clear();
                if (amb) { m.reason = "Several identical displays match; choose which one to use"; setStatus(m, RouteStatus::NeedsConfirmation, m.reason, ev, RT::NeedsConfirmation); }
                else setStatus(m, RouteStatus::Disconnected, "Mirror display is not connected", ev, RT::Disconnected);
            } else if (occupied.count(conn)) {
                m.bound.clear();
                m.reason = "This display is already showing the controls";
                setStatus(m, RouteStatus::NeedsConfirmation, m.reason, ev, RT::NeedsConfirmation);
            } else {
                const PhysicalDisplay* d = byConnector(conn);
                const bool was = m.status == RouteStatus::Active && m.bound == conn;
                applyViewport(m, *d);
                if (!was) ++m.generation;
                m.bound = conn;
                occupied.insert(conn);
                setStatus(m, RouteStatus::Active, "Controls mirror active", ev, RT::Reattached);
            }
        }
    }

    // ---- visual outputs (ascending id) ----
    int activeVisual = 0, activeIndependent = 0;
    for (std::size_t i = 2; i < outs_.size(); ++i) {
        Out& o = outs_[i];
        if (o.identity.empty()) {
            o.bound.clear();
            setStatus(o, RouteStatus::Unassigned, topo_.size() <= 1 ? "Needs a second display" : "Not assigned", ev, RT::Disconnected);
            continue;
        }
        bool amb = false;
        o.suggestedHint.clear();
        const std::string conn = resolve(o, &amb);
        if (conn.empty()) {
            o.bound.clear();
            if (amb) {
                o.reason = "Two or more connected displays look identical; choose which one this output should use. It stays idle until you confirm.";
                for (const auto& d : topo_) if (d.identity == o.identity && d.connector == o.connectorHint) o.suggestedHint = d.connector;
                setStatus(o, RouteStatus::NeedsConfirmation, o.reason, ev, RT::NeedsConfirmation);
            } else {
                setStatus(o, RouteStatus::Disconnected, "Its display is not connected. Controls were not moved.", ev, RT::Disconnected);
            }
            continue;
        }
        const bool wasHere = o.status == RouteStatus::Active && o.bound == conn;
        const PhysicalDisplay* d = byConnector(conn);
        if (conn == controlsBound_) {
            // Never draw audience content over the controls.
            o.bound = conn;
            if (o.status == RouteStatus::Active || o.status == RouteStatus::Suspended) {
                setStatus(o, RouteStatus::Suspended, "Paused: the controls are using this display", ev, RT::Suspended);
            } else {
                o.reason = "This display is currently showing the controls";
                o.bound.clear();
                setStatus(o, RouteStatus::NeedsConfirmation, o.reason, ev, RT::NeedsConfirmation);
            }
            occupied.insert(conn);
            continue;
        }
        if (occupied.count(conn)) {
            o.bound.clear();
            o.reason = "This display is already used by another output";
            setStatus(o, RouteStatus::NeedsConfirmation, o.reason, ev, RT::NeedsConfirmation);
            continue;
        }
        // capability limits (lower ids win)
        const bool indep = o.kind == RouteKind::Independent;
        if (activeVisual + 1 > cap_.maxVisualOutputs || (indep && activeIndependent + 1 > cap_.maxIndependentOutputs)) {
            o.bound.clear();
            o.reason = "Disabled: " + (cap_.reason.empty() ? std::string("this machine's measured capability does not allow more outputs") : cap_.reason);
            setStatus(o, RouteStatus::Disabled, o.reason, ev, RT::Disabled);
            continue;
        }
        applyViewport(o, *d);
        if (!wasHere) {
            ++o.generation;
            o.blackout = o.blackout || blackoutAll_;     // a returning output never shows content during a blackout
        }
        o.bound = conn;
        o.connectorHint = conn;
        const RT type = o.status == RouteStatus::Suspended ? RT::Resumed : (o.status == RouteStatus::Disabled ? RT::Enabled : RT::Reattached);
        setStatus(o, RouteStatus::Active, "Showing on " + mode(d->width, d->height), ev, type);
        occupied.insert(conn);
        ++activeVisual;
        if (indep) ++activeIndependent;
    }

    if (fingerprint() != before) ++version_;
}

// ---- commands ------------------------------------------------------------------------------
Status DisplayRouter::assignCommon(OutputId id, const std::string& identity, const std::string& connector) {
    Out* o = mut(id);
    if (!o) return invalid("unknown output");
    if (identity.empty() || identity.size() > 256 || connector.size() > 256) return invalid("display identity missing or too long");
    const PhysicalDisplay* d = byConnector(connector);
    if (!d || d->identity != identity) return invalid("that display is not connected");
    for (const Out& other : outs_) {
        if (other.id == id) continue;
        if (other.status == RouteStatus::Active && other.bound == connector)
            return invalid("that display already hosts another output; unassign it first");
    }
    if (id != kControlsOutputId && connector == controlsBound_) return invalid("that display hosts the controls");
    o->identity = identity;
    o->connectorHint = connector;
    o->pendingBind = connector;
    o->status = RouteStatus::Unassigned;
    return okStatus();
}

Status DisplayRouter::assignControls(const std::string& identity, const std::string& connector) {
    Status s = assignCommon(kControlsOutputId, identity, connector);
    if (!s.ok()) return s;
    // moving the controls: any visual route that was on the new controls display is paused by reconcile
    reconcile(nullptr);
    return okStatus();
}
Status DisplayRouter::assignMirror(const std::string& identity, const std::string& connector) {
    Status s = assignCommon(kMirrorOutputId, identity, connector);
    if (!s.ok()) return s;
    reconcile(nullptr);
    return okStatus();
}
Status DisplayRouter::assignOutput(OutputId id, const std::string& identity, const std::string& connector) {
    const Out* o = get(id);
    if (!o || o->controls) return invalid("not a visual output");
    Status s = assignCommon(id, identity, connector);
    if (!s.ok()) return s;
    reconcile(nullptr);
    return okStatus();
}

Status DisplayRouter::unassign(OutputId id) {
    Out* o = mut(id);
    if (!o) return invalid("unknown output");
    o->identity.clear(); o->connectorHint.clear(); o->bound.clear(); o->pendingBind.clear(); o->reason.clear();
    o->status = RouteStatus::Unassigned;
    ++o->generation;
    reconcile(nullptr);
    return okStatus();
}

Status DisplayRouter::confirm(OutputId id, const std::string& connector) {
    Out* o = mut(id);
    if (!o) return invalid("unknown output");
    if (o->status != RouteStatus::NeedsConfirmation && o->status != RouteStatus::Disconnected)
        return invalid("this output does not need confirmation");
    if (o->identity.empty()) return invalid("this output has no assignment");
    const PhysicalDisplay* d = byConnector(connector);
    if (!d || d->identity != o->identity) return invalid("that display does not match this output's assignment");
    for (const Out& other : outs_)
        if (other.id != id && other.status == RouteStatus::Active && other.bound == connector) return invalid("that display already hosts another output");
    if (!o->controls && connector == controlsBound_) return invalid("that display hosts the controls");
    o->pendingBind = connector;
    o->connectorHint = connector;
    reconcile(nullptr);
    return o->status == RouteStatus::Active ? okStatus() : invalid("could not activate the output on that display");
}

Status DisplayRouter::setOverlay(OutputId id, const OverlayPolicy& p) {
    const std::string f0 = fingerprint();
    if (id == kAllOutputs) {
        for (std::size_t i = 2; i < outs_.size(); ++i) outs_[i].overlay = p;
    } else {
        Out* o = mut(id);
        if (!o) return invalid("unknown output");
        if (o->controls) return invalid("controls overlays are fixed");
        o->overlay = p;
    }
    reconcile(nullptr);
    if (fingerprint() != f0) ++version_;
    return okStatus();
}

Status DisplayRouter::setBpmKeyVisible(OutputId id, bool on) {
    const std::string f0 = fingerprint();
    if (id == kAllOutputs) {
        for (std::size_t i = 2; i < outs_.size(); ++i) outs_[i].overlay.showBpmKey = on;
    } else {
        Out* o = mut(id);
        if (!o) return invalid("unknown output");
        if (o->controls) return invalid("controls overlays are fixed");
        o->overlay.showBpmKey = on;
    }
    reconcile(nullptr);
    if (fingerprint() != f0) ++version_;
    return okStatus();
}

Status DisplayRouter::setDiagnostics(OutputId id, bool on) {
    const std::string f0 = fingerprint();
    Out* o = mut(id);
    if (!o) return invalid("unknown output");
    if (o->controls) return invalid("controls always show diagnostics");
    o->overlay.showDiagnostics = on;
    reconcile(nullptr);
    if (fingerprint() != f0) ++version_;
    return okStatus();
}

void DisplayRouter::setFinalBlackout(OutputId target, bool on) {
    const std::string f0 = fingerprint();
    if (target == kAllOutputs) {
        blackoutAll_ = on;
        for (std::size_t i = 2; i < outs_.size(); ++i) outs_[i].blackout = on;
    } else {
        Out* o = mut(target);
        if (o && !o->controls) o->blackout = on;     // controls are never blacked out
    }
    reconcile(nullptr);
    if (fingerprint() != f0) ++version_;
}

// ---- queries -------------------------------------------------------------------------------
RouteInfo DisplayRouter::toInfo(const Out& o) const {
    RouteInfo r;
    r.label = o.label;
    r.isControls = o.controls && !o.mirror;
    r.isMirror = o.mirror;
    r.status = o.status;
    r.statusText = o.statusText;
    r.boundConnector = o.bound;
    r.suggestedConnector = o.status == RouteStatus::NeedsConfirmation ? o.suggestedHint : std::string();
    r.finalBlackout = !o.controls && (o.blackout || o.status == RouteStatus::Suspended);
    r.idle = o.status != RouteStatus::Active;
    OutputRoute& g = r.route;
    g.logicalId = o.id; g.kind = o.kind; g.displayIdentity = o.identity; g.generation = o.generation;
    g.viewportW = o.vw; g.viewportH = o.vh; g.rotationDeg = o.rot; g.duplicateOf = o.duplicateOf;
    if (o.controls) {
        g.showBpmKey = true; g.showText = true; g.showBranding = false; g.showDiagnostics = true;
    } else {
        g.showBpmKey = o.overlay.showBpmKey; g.showText = o.overlay.showText; g.showBranding = o.overlay.showBranding;
        g.showDiagnostics = o.overlay.showDiagnostics;
    }
    g.connected = o.status == RouteStatus::Active;
    return r;
}

std::vector<RouteInfo> DisplayRouter::routes() const {
    std::vector<RouteInfo> v;
    for (const Out& o : outs_) v.push_back(toInfo(o));
    return v;
}

std::optional<RouteInfo> DisplayRouter::find(OutputId id) const {
    const Out* o = get(id);
    if (!o) return std::nullopt;
    return toInfo(*o);
}

ViewportClass classifyViewport(int width, int height) {
    ViewportClass v;
    if (width <= 0 || height <= 0) return v;
    v.aspect = static_cast<double>(width) / static_cast<double>(height);
    auto near = [&](double r) { return v.aspect > r * 0.98 && v.aspect < r * 1.02; };
    if (v.aspect < 1.0) v.kind = ViewportClass::Kind::Portrait;
    else if (near(4.0 / 3.0)) v.kind = ViewportClass::Kind::Ratio4x3;
    else if (near(16.0 / 9.0)) v.kind = ViewportClass::Kind::Ratio16x9;
    else if (near(16.0 / 10.0)) v.kind = ViewportClass::Kind::Ratio16x10;
    return v;
}

std::vector<PhysicalDisplay> DisplayRouter::unassignedDisplays() const {
    std::vector<PhysicalDisplay> v;
    for (const PhysicalDisplay& d : topo_) {
        bool used = false;
        for (const Out& o : outs_) if (o.bound == d.connector) used = true;
        if (!used) v.push_back(d);
    }
    return v;
}

bool DisplayRouter::controlsDisplay(PhysicalDisplay* out) const {
    if (controlsBound_.empty()) return false;
    const PhysicalDisplay* d = byConnector(controlsBound_);
    if (!d) return false;
    if (out) *out = *d;
    return true;
}

std::string DisplayRouter::explain(OutputId id) const {
    const Out* o = get(id);
    if (!o) return "Unknown output";
    switch (o->status) {
        case RouteStatus::Unassigned:
            if (o->controls) return "Not assigned";
            if (topo_.size() <= 1) return "Only one display is connected; a separate audience output needs a second display.";
            return "No display is assigned. " + std::to_string(unassignedDisplays().size()) + " unassigned display(s) are available.";
        case RouteStatus::Active: return "Showing on " + mode(o->vw, o->vh) + ".";
        case RouteStatus::Disconnected: return "Its display is not connected. Reconnect it or assign another display; the controls were not moved.";
        case RouteStatus::NeedsConfirmation: return o->reason;
        case RouteStatus::Suspended: return "Paused because the controls moved onto this display after their own display was lost.";
        case RouteStatus::Disabled: return o->reason;
    }
    return "";
}

IdentifySession DisplayRouter::identify(Ns now, Ns duration) const {
    IdentifySession s;
    s.active = !topo_.empty();
    s.startedAt = now;
    s.expiresAt = now + std::min<Ns>(std::max<Ns>(duration, 2 * kNsPerSec), 60 * kNsPerSec);
    std::vector<const PhysicalDisplay*> ds;
    for (const auto& d : topo_) ds.push_back(&d);
    std::sort(ds.begin(), ds.end(), [](const auto* a, const auto* b) { return a->connector < b->connector; });
    int n = 0;
    for (const PhysicalDisplay* d : ds) {
        IdentifyLabel l;
        l.connector = d->connector;
        l.ordinal = ++n;
        l.title = "Display " + std::to_string(n);
        std::string role = "Not assigned";
        for (const Out& o : outs_) {
            if (o.bound != d->connector) continue;
            if (o.controls) role = o.mirror ? "Controls (mirror)" : "Controls";
            else role = (o.kind == RouteKind::Audience ? "Audience: " : "Output: ") + o.label;
        }
        l.subtitle = role + "  " + mode(d->width, d->height);
        s.labels.push_back(l);
    }
    return s;
}

std::vector<std::string> DisplayRouter::invariantViolations() const {
    std::vector<std::string> v;
    std::set<std::string> seen;
    for (const Out& o : outs_) {
        if (o.status == RouteStatus::Active) {
            if (!byConnector(o.bound)) v.push_back("active route bound to a display that is not present: " + o.label);
            if (!seen.insert(o.bound).second) v.push_back("two active routes share one display");
            if (!o.controls && o.bound == controlsBound_) v.push_back("audience route is on the controls display: " + o.label);
        }
        if (o.status == RouteStatus::Disconnected && !o.controls && !o.bound.empty()) v.push_back("disconnected route still bound: " + o.label);
        if (o.status == RouteStatus::NeedsConfirmation && !o.bound.empty() && !o.controls) v.push_back("unconfirmed route is not idle: " + o.label);
        if (o.controls && o.blackout) v.push_back("controls flagged for blackout");
        if (o.controls && o.status == RouteStatus::Active && o.bound.empty()) v.push_back("controls active without a display");
    }
    return v;
}

}  // namespace ss::router
