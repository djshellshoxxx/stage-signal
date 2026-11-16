#include "App/Benchmark/SoftwareBenchmark.h"
#include <chrono>
#include <cmath>
#include <juce_core/juce_core.h>
#include "Engine/Media/ImageDecode.h"
#include "ss/compose/compositor.hpp"
#include "ss/base/sha256.hpp"
#include "ss/render/scene.hpp"

namespace ss::app {
namespace {
class RealBenchTime final : public ss::perf::IBenchTime {
public:
    Ns nowMonotonic() const override { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    Ns wallUnixNs() const override { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    void waitUntil(Ns at) override { std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(at))); }
};
class SceneWorkload final : public ss::perf::IBenchWorkload {
public:
    explicit SceneWorkload(ss::perf::IBenchTime& time) : time_(time) {}
    std::vector<ss::perf::SinkDecl> sinks() const override { return {{ss::perf::SinkKind::Mock, "software frames discarded", ""}}; }
    double presentationRateHz() const override { return 25.; }
    ss::Status prepare(const ss::perf::ScenarioDescriptor& scenario, std::uint64_t seed) override {
        outputs_.clear();
        const int count = scenario.id == "BEN-02" ? 3 : 1;
        for (int i = 0; i < count; ++i) {
            auto output = std::make_unique<Output>();
            output->scene = ss::render::makeScene(i == 2 ? ss::model::SceneKind::ColorField : ss::model::SceneKind::SpectrumBars);
            output->scene->prepare(800, 600, seed);
            output->pixels = PixelBuffer(800, 600);
            auto status = output->compositor.prepare(800, 600);
            if (!status.ok()) return status;
            auto text = rasterText("StageSignal benchmark", 600, 64);
            if (!text.ok()) return text.error();
            ss::compose::TextItem item;
            item.id = 1; item.utf8 = "StageSignal benchmark"; item.durationSec = 0;
            item.bitmap = std::make_shared<const PixelBuffer>(std::move(text.value()));
            status = output->text.publish(item, time_.nowMonotonic());
            if (!status.ok()) return status;
            outputs_.push_back(std::move(output));
        }
        next_ = time_.nowMonotonic();
        return ss::okStatus();
    }
    void step(Ns now, ss::perf::RunRecorder& recorder) override {
        if (now < next_) return;
        next_ = now + msToNs(40);
        const auto began = std::chrono::steady_clock::now();
        FeatureSnapshot features;
        features.silent = false;
        for (auto& band : features.band) band = .5f;
        ss::render::AudioVisualInput audio;
        for (auto& bin : audio.spectrum) bin = .5f;
        for (auto& output : outputs_) {
            ss::render::SceneInput scene;
            scene.timeNs = now; scene.features = &features; scene.audio = &audio; scene.strobingDisabled = true;
            output->scene->render(scene, output->pixels);
            ss::compose::FrameInputs input;
            input.nowNs = now; input.scene.a = &output->pixels; input.text = &output->text;
            const auto frame = output->compositor.compose(input);
            if (!frame.ok()) { recorder.miss(ss::perf::MissKind::Render); continue; }
            output->compositor.release(frame);
        }
        const Ns duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count();
        recorder.frame(time_.nowMonotonic(), duration);
        recorder.presentationDeadline(duration <= msToNs(40));
    }
private:
    struct Output { std::unique_ptr<ss::render::SceneRenderer> scene; PixelBuffer pixels; ss::compose::Compositor compositor; ss::compose::TextLayer text; };
    ss::perf::IBenchTime& time_;
    std::vector<std::unique_ptr<Output>> outputs_;
    Ns next_ = 0;
};
}  // namespace
ss::Result<ss::perf::SuiteReport> runSoftwareBenchmark(ss::perf::IBenchTime& time, ss::perf::RunnerConfig config,
                                                     const std::atomic<bool>& cancel) {
    config.cancel = &cancel;
    SceneWorkload basic(time), multiple(time);
    ss::perf::BenchEnvironment environment;
    if (auto stream = juce::File::getSpecialLocation(juce::File::currentExecutableFile).createInputStream()) {
        ss::Sha256 hash;
        std::array<char, 65536> buffer{};
        for (int count = stream->read(buffer.data(), static_cast<int>(buffer.size())); count > 0;
             count = stream->read(buffer.data(), static_cast<int>(buffer.size()))) hash.update(buffer.data(), static_cast<std::size_t>(count));
        environment.appHash = ss::Sha256::hex(hash.finish());
    } else environment.appHash = "unavailable";
    environment.dependencyHashes = {{"JUCE", "ea77444750de45e036e0279bb0291144bf8e3688"},
                                    {"clap-juce-extensions", "7adee3a1bd4684d4caa5601100e364abccff4b4d"}};
    environment.os = juce::SystemStats::getOperatingSystemName().toStdString();
    environment.cpu = juce::SystemStats::getCpuModel().toStdString();
    environment.logicalProcessors = static_cast<unsigned>(juce::SystemStats::getNumCpus());
    environment.compositions = {{800, 600}};
    environment.powerPlan = "unavailable"; environment.gpu = "software renderer";
    ss::perf::BenchmarkRunner runner(time, config);
    return runner.runSuite(environment, {
        {"BEN-01", &basic, ""}, {"BEN-02", &multiple, ""},
        {"BEN-03", nullptr, "Camera/video decode workload not qualified by this software-only run"},
        {"BEN-04", nullptr, "Encoder workload not included in software-only benchmark"},
        {"BEN-05", nullptr, "Broadcast workload not included; no network outputs opened"},
        {"BEN-06", nullptr, "Full-show stress qualification requires the documented representative show"}}, 1);
}
bool SoftwareBenchmark::start(bool quick) {
    { std::lock_guard<std::mutex> lock(mutex_); if (view_.running) return false; }
    if (worker_.joinable()) worker_.join();
    cancel_.store(false);
    { std::lock_guard<std::mutex> lock(mutex_); view_.running = true; view_.note = "Software-only benchmark: 15 s warm-up + 60 s per scenario. No live outputs."; }
    worker_ = std::thread([this, quick] {
        try {
            RealBenchTime time;
            ss::perf::RunnerConfig config;
            config.runs = quick ? 1 : 3;
            auto result = runSoftwareBenchmark(time, config, cancel_);
            std::lock_guard<std::mutex> lock(mutex_);
            if (result.ok()) {
                result.value().capturedAtUnixMs = time.wallUnixNs() / kNsPerMs;
                report_ = ss::perf::toJson(result.value());
                view_.profileHash = result.value().profileHash();
                view_.note = "BEN-01/BEN-02 measured. Unqualified: device/latency/system metrics and remaining workloads are unavailable.";
            } else { view_.note = result.error().detail; report_.clear(); }
            view_.running = false; ++view_.generation;
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.running = false; view_.note = "Benchmark failed; no qualification result was saved."; ++view_.generation;
        }
    });
    return true;
}
void SoftwareBenchmark::stop() { cancel(); if (worker_.joinable()) worker_.join(); }
BenchmarkView SoftwareBenchmark::view() const { std::lock_guard<std::mutex> lock(mutex_); return view_; }
std::string SoftwareBenchmark::report() const { std::lock_guard<std::mutex> lock(mutex_); return report_; }
}  // namespace ss::app
