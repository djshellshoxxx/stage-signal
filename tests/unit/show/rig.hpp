// Test rig for the show engine: deterministic clock, document builders, event bookkeeping.
#pragma once
#include <vector>
#include "doctest/doctest.h"
#include "ss/base/actions.hpp"
#include "ss/base/time.hpp"
#include "ss/show/engine.hpp"

namespace sst {
using namespace ss;
using namespace ss::show;

inline model::SceneDef mkScene(std::uint32_t id, model::EnergyTier tier = model::EnergyTier::Any, bool strobe = false) {
    model::SceneDef s;
    s.id = id; s.name = "s" + std::to_string(id);
    s.kind = static_cast<model::SceneKind>(id % 12);
    s.tier = tier; s.strobe = strobe; s.seed = 1000 + id;
    return s;
}

inline model::CueDef mkCue(std::uint32_t id, std::uint32_t scene, double dur, model::TimeBase tb = model::TimeBase::Seconds,
                           bool wait = false, double trSec = 0.0) {
    model::CueDef c;
    c.id = id; c.name = "c" + std::to_string(id); c.sceneId = scene; c.duration = dur; c.timeBase = tb;
    c.transition = model::TransitionKind::Cut; c.transitionDuration = trSec; c.waitForTrigger = wait;
    return c;
}

inline model::ShowDocument mkDoc(int nScenes, std::uint64_t seed = 42, int minDwell = 2, int history = 3) {
    model::ShowDocument d;
    for (int i = 1; i <= nScenes; ++i) d.scenes.push_back(mkScene(static_cast<std::uint32_t>(i)));
    d.seed = seed; d.minDwellSec = minDwell; d.repeatHistory = history;
    d.mode = ShowMode::Auto;
    return d;
}

inline Tunables fastTun() {
    Tunables t;
    t.autoIntervalSec = 2.0;
    t.quantiseAutoToBar = false;
    return t;
}

struct Rig {
    FakeClock clock;
    ShowEngine eng;
    Ns t = 0;
    FeatureSnapshot f;
    ShowSnapshot snap;
    std::vector<ShowEvent> all;

    explicit Rig(const model::ShowDocument& d, Tunables tun = fastTun()) : eng(&clock, tun) {
        REQUIRE(eng.loadDocument(d).ok());
        step(0);
    }
    ShowSnapshot step(Ns dt = msToNs(100)) {
        t += dt; clock.set(t);
        snap = eng.tick(t, f);
        for (auto& e : snap.events) all.push_back(e);
        return snap;
    }
    void run(Ns total, Ns dt = msToNs(100)) {
        const Ns end = t + total;
        while (t < end) step(std::min(dt, end - t));
    }
    ActionResult act(ActionId a, double value = 0.0, OutputId target = kAllOutputs, TriggerKind tr = TriggerKind::Immediate,
                     Ns trigTime = 0, Revision expected = 0, std::uint32_t fixture = 0) {
        Action x;
        x.actionId = id(a); x.value = value; x.target = target; x.trigger = tr; x.triggerTime = trigTime;
        x.expectedRevision = expected; x.fixtureId = fixture;
        return eng.submit(x);
    }
    // submit and tick once so the action has been applied
    ActionResult actNow(ActionId a, double value = 0.0, OutputId target = kAllOutputs) {
        auto r = act(a, value, target);
        step(0);
        return r;
    }
    int count(EventKind k) const { int n = 0; for (auto& e : all) if (e.kind == k) ++n; return n; }
    std::vector<std::uint32_t> sceneSeq() const {
        std::vector<std::uint32_t> v;
        for (auto& e : all) if (e.kind == EventKind::SceneChanged && e.output == kPrimaryOutput) v.push_back(e.sceneId);
        return v;
    }
    std::uint32_t scene(OutputId o = kPrimaryOutput) const { return snap.output(o)->sceneId; }
};

}  // namespace sst
