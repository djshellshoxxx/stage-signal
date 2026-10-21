#include "ss/perf/metrics.hpp"
#include <algorithm>
#include <cmath>
#include "ss/perf/limits.hpp"

namespace ss::perf {

const Metric* MetricSet::find(const std::string& name) const {
    for (const Metric& m : items_) if (m.name == name) return &m;
    return nullptr;
}

void MetricSet::put(const std::string& name, double value, bool avail) {
    for (Metric& m : items_) {
        if (m.name == name) { m.value = avail ? value : 0.0; m.available = avail; return; }
    }
    items_.push_back(Metric{name, avail ? value : 0.0, avail});
}

bool metricHigherIsBetter(const std::string& name) {
    return name == "fps.mean" || name == "fps.windows.share" || name == "mem.free.min" || name == "frames.presented" ||
           name == "fps.windows.meeting";
}

SampleWindow::SampleWindow(std::size_t capacity) {
    const std::size_t c = std::min<std::size_t>(std::max<std::size_t>(capacity, 1), kMaxWindowSamples);
    buf_.assign(c, 0.0);
    scratch_.assign(c, 0.0);
}

void SampleWindow::add(double v) {
    buf_[head_] = v;
    head_ = (head_ + 1) % buf_.size();
    if (count_ < buf_.size()) ++count_;
}

void SampleWindow::clear() { head_ = 0; count_ = 0; }

Percentiles SampleWindow::percentiles() const {
    Percentiles p;
    if (count_ == 0) return p;
    // Samples in ring order do not matter for rank statistics.
    std::copy(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(count_), scratch_.begin());
    auto first = scratch_.begin();
    auto last = scratch_.begin() + static_cast<std::ptrdiff_t>(count_);
    std::sort(first, last);
    auto rank = [&](double q) {  // nearest-rank
        std::size_t r = static_cast<std::size_t>(std::ceil(q * static_cast<double>(count_)));
        if (r < 1) r = 1;
        if (r > count_) r = count_;
        return scratch_[r - 1];
    };
    double sum = 0;
    for (std::size_t i = 0; i < count_; ++i) sum += scratch_[i];
    p.available = true;
    p.count = count_;
    p.p50 = rank(0.50);
    p.p95 = rank(0.95);
    p.p99 = rank(0.99);
    p.max = scratch_[count_ - 1];
    p.mean = sum / static_cast<double>(count_);
    return p;
}

CpuSample normaliseCpu(const std::vector<double>& perCoreBusyPct, Ns processCpuNsDelta, Ns wallNsDelta,
                       unsigned logicalProcessors) {
    CpuSample s;
    if (perCoreBusyPct.empty() || wallNsDelta <= 0 || logicalProcessors == 0) return s;  // unavailable
    double sum = 0, mx = 0;
    for (double v : perCoreBusyPct) {
        const double c = std::min(100.0, std::max(0.0, v));
        sum += c;
        mx = std::max(mx, c);
    }
    s.available = true;
    s.systemPct = sum / static_cast<double>(perCoreBusyPct.size());
    s.busiestCorePct = mx;
    const double proc = 100.0 * static_cast<double>(std::max<Ns>(0, processCpuNsDelta)) /
                        (static_cast<double>(wallNsDelta) * static_cast<double>(logicalProcessors));
    s.processPct = std::min(100.0, proc);
    return s;
}

RunRecorder::RunRecorder(double presentationRateHz, std::size_t cap)
    : rateHz_(presentationRateHz > 0 ? presentationRateHz : 1.0),
      minFrames_(std::max(1, static_cast<int>(std::floor(presentationRateHz)) - 1)),
      interval_(cap), render_(cap), ui_(cap), scene_(cap), onset_(cap), cpuProc_(4096), cpuSys_(4096), cpuBusy_(4096),
      ws_(4096), commit_(4096) {}

void RunRecorder::begin(Ns startNs) { start_ = startNs; last_ = -1; }

void RunRecorder::frame(Ns presentedAt, Ns renderNs) {
    const Ns rel = presentedAt < start_ ? 0 : presentedAt - start_;
    const std::int64_t idx = rel / kNsPerSec;
    while (curIdx_ < idx) {
        ++winTotal_;
        if (curCount_ >= static_cast<std::uint64_t>(minFrames_)) ++winMeeting_;
        curCount_ = 0;
        ++curIdx_;
    }
    ++curCount_;
    ++frames_;
    if (last_ >= 0 && presentedAt >= last_) interval_.add(nsToMs(presentedAt - last_));
    last_ = presentedAt;
    render_.add(nsToMs(renderNs));
    const Ns period = static_cast<Ns>(1e9 / rateHz_);
    if (renderNs > period) ++miss_[static_cast<int>(MissKind::Render)];
}

void RunRecorder::presentationDeadline(bool met) {
    deadlinesRecorded_ = true;
    ++due_;
    if (!met) { ++missedPres_; ++miss_[static_cast<int>(MissKind::Presentation)]; }
}

void RunRecorder::miss(MissKind kind, std::uint64_t n) {
    if (kind == MissKind::Presentation) { missedPres_ += n; }
    miss_[static_cast<int>(kind)] += n;
}

void RunRecorder::queueOccupancy(const std::string& name, double occ) {
    const double o = std::min(1.0, std::max(0.0, occ));
    for (Q& q : queues_) {
        if (q.name == name) { q.max = std::max(q.max, o); q.sum += o; ++q.n; return; }
    }
    if (queues_.size() >= kMaxNamedQueues) return;  // bounded: extra queue names are ignored
    queues_.push_back(Q{name, o, o, 1});
}

void RunRecorder::cpu(const CpuSample& s) {
    if (!s.available) return;
    cpuProc_.add(s.processPct); cpuSys_.add(s.systemPct); cpuBusy_.add(s.busiestCorePct);
    cpuProcSum_ += s.processPct; cpuSysSum_ += s.systemPct; cpuBusySum_ += s.busiestCorePct;
    ++cpuN_;
}

void RunRecorder::mem(const MemSample& s) {
    if (s.workingSetAvailable) { ws_.add(static_cast<double>(s.workingSetBytes)); wsPeak_ = std::max(wsPeak_, s.workingSetBytes); ++wsN_; }
    if (s.commitAvailable) { commit_.add(static_cast<double>(s.privateCommitBytes)); commitPeak_ = std::max(commitPeak_, s.privateCommitBytes); ++commitN_; }
    if (s.hostAvailable) { hostPeak_ = std::max(hostPeak_, s.hostWorkingSetBytes); ++hostN_; }
    if (s.freeAvailable) { freeMin_ = std::min(freeMin_, s.availablePhysBytes); ++freeN_; }
}

void RunRecorder::uiAck(Ns l) { ui_.add(nsToMs(l)); }
void RunRecorder::sceneResponse(Ns l) { scene_.add(nsToMs(l)); }
void RunRecorder::audioToVisualOnset(Ns l) { onset_.add(nsToMs(l)); }

namespace {
void putPct(MetricSet& m, const std::string& base, const Percentiles& p, bool withP50) {
    if (!p.available) {
        if (withP50) m.setUnavailable(base + ".p50.ms");
        m.setUnavailable(base + ".p95.ms"); m.setUnavailable(base + ".p99.ms"); m.setUnavailable(base + ".max.ms");
        return;
    }
    if (withP50) m.set(base + ".p50.ms", p.p50);
    m.set(base + ".p95.ms", p.p95); m.set(base + ".p99.ms", p.p99); m.set(base + ".max.ms", p.max);
}
}  // namespace

MetricSet RunRecorder::finish(Ns endNs) const {
    MetricSet m;
    const Ns dur = endNs > start_ ? endNs - start_ : 0;
    const double durSec = nsToSec(dur);
    m.set("duration.s", durSec);
    m.set("frames.presented", static_cast<double>(frames_));
    if (durSec > 0) m.set("fps.mean", static_cast<double>(frames_) / durSec); else m.setUnavailable("fps.mean");

    // Close the remaining whole 1 s windows without mutating the live counters.
    std::uint64_t total = winTotal_, meeting = winMeeting_;
    const std::int64_t lastFull = dur / kNsPerSec;
    if (curIdx_ < lastFull) {
        ++total;
        if (curCount_ >= static_cast<std::uint64_t>(minFrames_)) ++meeting;
        total += static_cast<std::uint64_t>(lastFull - curIdx_ - 1);   // empty windows: zero frames
    }
    m.set("fps.windows.total", static_cast<double>(total));
    m.set("fps.windows.meeting", static_cast<double>(meeting));
    if (total > 0) m.set("fps.windows.share", static_cast<double>(meeting) / static_cast<double>(total));
    else m.setUnavailable("fps.windows.share");

    putPct(m, "interval", interval_.percentiles(), true);
    putPct(m, "render", render_.percentiles(), true);

    m.set("miss.render", static_cast<double>(miss_[0]));
    m.set("miss.presentation", static_cast<double>(miss_[1]));
    m.set("miss.decoder", static_cast<double>(miss_[2]));
    m.set("miss.encoder", static_cast<double>(miss_[3]));
    if (deadlinesRecorded_ && due_ > 0) {
        m.set("presentation.due", static_cast<double>(due_));
        m.set("miss.presentation.fraction", static_cast<double>(missedPres_) / static_cast<double>(due_));
    } else {
        m.setUnavailable("presentation.due");
        m.setUnavailable("miss.presentation.fraction");
    }

    for (const Q& q : queues_) {
        m.set("queue." + q.name + ".max", q.max);
        m.set("queue." + q.name + ".mean", q.n ? q.sum / static_cast<double>(q.n) : 0.0);
    }

    if (cpuN_ > 0) {
        const double n = static_cast<double>(cpuN_);
        m.set("cpu.process.avg", cpuProcSum_ / n); m.set("cpu.process.p95", cpuProc_.percentiles().p95);
        m.set("cpu.system.avg", cpuSysSum_ / n);   m.set("cpu.system.p95", cpuSys_.percentiles().p95);
        m.set("cpu.busiest.avg", cpuBusySum_ / n); m.set("cpu.busiest.p95", cpuBusy_.percentiles().p95);
    } else {
        for (const char* k : {"cpu.process.avg", "cpu.process.p95", "cpu.system.avg", "cpu.system.p95", "cpu.busiest.avg", "cpu.busiest.p95"})
            m.setUnavailable(k);
    }

    if (wsN_ > 0) { m.set("mem.ws.steady", ws_.percentiles().p50); m.set("mem.ws.peak", static_cast<double>(wsPeak_)); }
    else { m.setUnavailable("mem.ws.steady"); m.setUnavailable("mem.ws.peak"); }
    if (commitN_ > 0) { m.set("mem.commit.steady", commit_.percentiles().p50); m.set("mem.commit.peak", static_cast<double>(commitPeak_)); }
    else { m.setUnavailable("mem.commit.steady"); m.setUnavailable("mem.commit.peak"); }
    if (hostN_ > 0) m.set("mem.host.peak", static_cast<double>(hostPeak_)); else m.setUnavailable("mem.host.peak");
    if (freeN_ > 0) m.set("mem.free.min", static_cast<double>(freeMin_)); else m.setUnavailable("mem.free.min");

    auto lat = [&](const char* name, const SampleWindow& w) {
        const Percentiles p = w.percentiles();
        if (p.available) m.set(name, p.p95); else m.setUnavailable(name);
    };
    lat("latency.ui.p95.ms", ui_);
    lat("latency.scene.p95.ms", scene_);
    lat("latency.onset.p95.ms", onset_);
    if (overrunsReported_) m.set("capture.overruns", static_cast<double>(overruns_)); else m.setUnavailable("capture.overruns");
    return m;
}

}  // namespace ss::perf
