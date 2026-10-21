// Shared test fixtures: a synthetic 60 s run and a fake benchmark workload driven by fake time.
#pragma once
#include <string>
#include <vector>
#include "ss/perf/benchmark.hpp"
#include "ss/perf/metrics.hpp"

namespace perftest {
using namespace ss;
using namespace ss::perf;

struct RunShape {
    double rate = 25.0;
    Ns renderNs = 20 * kNsPerMs;
    double cpuPct = 40.0;
    std::uint64_t ws = 60 * kMiB, commit = 62 * kMiB, host = 40 * kMiB, freeBytes = 200 * kMiB;
    Ns uiAck = 30 * kNsPerMs, scene = 80 * kNsPerMs, onset = 90 * kNsPerMs;
    bool recordDeadlines = true;
    bool cpuAvailable = true, memAvailable = true, overrunsMeasured = true;
    Ns stallAtSec = -1;                // inject one long gap before the frame at this second
    Ns stallLen = 0;
    int missEvery = 0;                 // every Nth presentation deadline missed (0 = never)
};

// Builds a 60 s run directly into a recorder.
inline MetricSet synthRun(const RunShape& s, Ns durationNs = 60 * kNsPerSec) {
    RunRecorder rec(s.rate);
    rec.begin(0);
    const Ns period = static_cast<Ns>(1e9 / s.rate);
    int n = 0;
    Ns t = period;
    bool stalled = false;
    while (t < durationNs) {
        if (!stalled && s.stallAtSec >= 0 && t >= s.stallAtSec * kNsPerSec) { t += s.stallLen; stalled = true; if (t >= durationNs) break; }
        rec.frame(t, s.renderNs);
        if (s.recordDeadlines) rec.presentationDeadline(!(s.missEvery && (++n % s.missEvery == 0)));
        t += period;
    }
    for (int sec = 1; sec <= durationNs / kNsPerSec; ++sec) {
        CpuSample c; c.available = s.cpuAvailable; c.processPct = s.cpuPct; c.systemPct = s.cpuPct; c.busiestCorePct = s.cpuPct;
        rec.cpu(c);
        MemSample m; m.workingSetAvailable = m.commitAvailable = m.hostAvailable = m.freeAvailable = s.memAvailable;
        m.workingSetBytes = s.ws; m.privateCommitBytes = s.commit; m.hostWorkingSetBytes = s.host; m.availablePhysBytes = s.freeBytes;
        rec.mem(m);
    }
    if (s.uiAck >= 0) for (int i = 0; i < 20; ++i) rec.uiAck(s.uiAck);
    if (s.scene >= 0) for (int i = 0; i < 20; ++i) rec.sceneResponse(s.scene);
    if (s.onset >= 0) for (int i = 0; i < 20; ++i) rec.audioToVisualOnset(s.onset);
    if (s.overrunsMeasured) rec.declareCaptureOverrunsMeasured();
    return rec.finish(durationNs);
}

// Fake workload: emits frames at its rate from `step`, no real work, no real sinks.
class FakeWorkload : public IBenchWorkload {
public:
    RunShape shape;
    std::vector<SinkDecl> declared;
    bool prepared = false, finished = false;
    int steps = 0;
    bool lowered = false;
    std::uint64_t lastSeed = 0;
    Status prepareStatus = okStatus();

    explicit FakeWorkload(RunShape s = {}) : shape(s) {}
    std::vector<SinkDecl> sinks() const override { return declared; }
    double presentationRateHz() const override { return shape.rate; }
    Status prepare(const ScenarioDescriptor&, std::uint64_t seed) override { prepared = true; lastSeed = seed; return prepareStatus; }
    void step(Ns now, RunRecorder& rec) override {
        ++steps;
        if (first_ < 0) { first_ = now; nextFrame_ = now; nextSec_ = now + kNsPerSec; }
        const Ns period = static_cast<Ns>(1e9 / shape.rate);
        while (nextFrame_ <= now) {
            rec.frame(nextFrame_, shape.renderNs);
            if (shape.recordDeadlines) rec.presentationDeadline(true);
            nextFrame_ += period;
        }
        while (nextSec_ <= now) {
            CpuSample c; c.available = shape.cpuAvailable; c.processPct = shape.cpuPct; c.systemPct = shape.cpuPct; c.busiestCorePct = shape.cpuPct;
            rec.cpu(c);
            MemSample m; m.workingSetAvailable = m.commitAvailable = m.hostAvailable = m.freeAvailable = shape.memAvailable;
            m.workingSetBytes = shape.ws; m.privateCommitBytes = shape.commit; m.hostWorkingSetBytes = shape.host; m.availablePhysBytes = shape.freeBytes;
            rec.mem(m);
            rec.uiAck(shape.uiAck); rec.sceneResponse(shape.scene); rec.audioToVisualOnset(shape.onset);
            rec.declareCaptureOverrunsMeasured();
            nextSec_ += kNsPerSec;
        }
    }
    void finish() override { finished = true; }
    bool sceneRateLowered() const override { return lowered; }
    // Called by the runner at the start of each phase through begin(); we re-arm on first step of a phase.
    void rearm() { first_ = -1; }
private:
    Ns first_ = -1, nextFrame_ = 0, nextSec_ = 0;
};

}  // namespace perftest
