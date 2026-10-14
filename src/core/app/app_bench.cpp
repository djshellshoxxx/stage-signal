#include "core/app/app_bench.h"
#include "render/renderer.h"
#include <chrono>
#include <cmath>
#include <thread>

namespace ss { namespace app {

namespace {
struct Workload { int outputs = 1; bool independent = false; bool camera = false; bool recEnc = false; bool castEnc = false; };

ScenarioOutcome runWorkload(ScenarioContext& c, const AppBenchInputs& in, const Workload& w) {
  const int W = in.width, H = in.height;
  std::vector<std::unique_ptr<Renderer>> rend;
  std::vector<Frame> outs((size_t)w.outputs);
  for (int i = 0; i < w.outputs; ++i) {
    rend.emplace_back(new Renderer());
    float scale = std::min(1.0f, std::sqrt((float)(800 * 600) / (float)std::max(1, W * H)));
    Status s = rend.back()->configure(W, H, std::max(0.1f, scale));
    if (!s.ok()) return ScenarioOutcome::Fail("renderer configure failed: " + s.detail);
    if (in.text) rend.back()->setTextRasterizer(in.text);
  }
  std::unique_ptr<IVideoEncoder> enc;
  if (w.recEnc || w.castEnc) {
    if (!in.encoders) return ScenarioOutcome::Skip("no encoder registry");
    VideoProfile vp; vp.codec = VideoCodec::MJPEG; vp.width = W; vp.height = H; vp.fpsNum = 25; vp.fpsDen = 1;
    Status s = in.encoders->create(vp, enc);
    if (!s.ok()) return ScenarioOutcome::Skip("encoder unavailable: " + s.detail);
  }
  if (w.castEnc) { Status b = c.sinks.broadcast.begin("127.0.0.1:9"); if (!b.ok()) return ScenarioOutcome::Fail(b.detail); }
  Frame cam; cam.resize(640, 480);
  for (int y = 0; y < 480; ++y) for (int x = 0; x < 640; ++x) cam.px[(size_t)y * 640 + x] = rgb(x * 255 / 640, y * 255 / 480, 128);
  const Ns period = 40 * kNsPerMs;
  Ns next = c.clock.nowNs();
  uint64_t frameNo = 0;
  while (c.timeLeft()) {
    if (in.cancel && in.cancel->load()) return ScenarioOutcome::Fail("benchmark cancelled");
    Ns now = c.clock.nowNs();
    FeatureSnapshot f; double t = nsToSec(now);
    f.silent = false; f.sequence = frameNo + 1; f.rmsDbMix = -18; f.rmsDb[0] = f.rmsDb[1] = -18;
    f.band[0] = (float)(0.5 + 0.5 * std::sin(t * 3)); f.band[1] = (float)(0.5 + 0.5 * std::sin(t * 5)); f.band[2] = (float)(0.5 + 0.5 * std::sin(t * 7));
    f.onset = (frameNo % 12) == 0; f.onsetStrength = f.onset ? 1.0f : 0.2f;
    f.tempo.valid = true; f.tempo.value = 120; f.tempo.confidence = 0.8f;
    auto t0 = std::chrono::steady_clock::now();
    for (int o = 0; o < w.outputs; ++o) {
      RenderState rs; rs.elapsedNs = now; rs.features = f;
      int sc = (int)((frameNo / 200) % kSceneCount);
      rs.a.scene = (w.independent && o == 1) ? (sc + 3) % kSceneCount : sc; rs.a.params = defaultParams(rs.a.scene); rs.a.seed = (uint32_t)c.sceneSeed; rs.a.timeNs = now;
      TextOverlay tx; tx.text = U"BENCHMARK 0123456789"; rs.texts.push_back(tx);
      if (w.camera) {
        rs.camera.frame = &cam; rs.camera.routed = true; rs.camera.t.mirror = true; rs.camera.t.tintAmount = 0.2f; rs.camera.t.posterize = true;
        rs.camera.t.pip = (o == 0); rs.camera.t.trailFrames = 4;
      }
      rend[(size_t)o]->render(outs[(size_t)o], rs);
    }
    if (enc) {
      EncodedPacket ep; bool have = false;
      Status es = enc->encode(outs[0], now, ep, have);
      if (!es.ok()) c.metrics.drop(DropCat::Encode);
      else if (have) {
        if (w.recEnc) { Status ws = c.sinks.recording.write(ep.data.data(), ep.data.size()); if (!ws.ok()) c.metrics.drop(DropCat::Encode); }
        if (w.castEnc) c.sinks.broadcast.sendPacket(ep.data.data(), ep.data.size());
      }
      c.metrics.queueSample("encoder", 0, 3);
    }
    c.metrics.renderTime((Ns)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    c.metrics.framePresented();
    next += period;
    if (in.fake) in.fake->advance(period);
    else { Ns rem = next - c.clock.nowNs(); if (rem > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(rem)); }
    c.metrics.deadline(in.fake ? false : c.clock.nowNs() > next + period / 4);
    if (!in.fake && c.clock.nowNs() > next + period) next = c.clock.nowNs();
    if (frameNo % 25 == 0) c.sampleCounters();
    ++frameNo;
  }
  return ScenarioOutcome::Done();
}
} // namespace

BenchResult runAppBenchmark(const AppBenchInputs& in) {
  BenchConfig bc; bc.durationScale = in.durationScale; bc.enabled = in.scenarios;
  IClock& clk = *in.clock;
  BenchmarkRunner run(clk, bc, in.cpu, in.mem);
  Workload w1; run.setScenario(BenchId::BEN01, [&](ScenarioContext& c) { return runWorkload(c, in, w1); });
  Workload w2; w2.outputs = 3; w2.independent = true; run.setScenario(BenchId::BEN02, [&](ScenarioContext& c) { return runWorkload(c, in, w2); });
  Workload w3; w3.camera = true; run.setScenario(BenchId::BEN03, [&](ScenarioContext& c) { return runWorkload(c, in, w3); });
  Workload w4; w4.recEnc = true; run.setScenario(BenchId::BEN04, [&](ScenarioContext& c) { return runWorkload(c, in, w4); });
  Workload w5; w5.castEnc = true; run.setScenario(BenchId::BEN05, [&](ScenarioContext& c) { return runWorkload(c, in, w5); });
  Workload w6; w6.outputs = 2; w6.independent = true; w6.recEnc = true; run.setScenario(BenchId::BEN06, [&](ScenarioContext& c) { return runWorkload(c, in, w6); });
  BenchEnvironment env;
  env.fixtureId = "app-synthetic"; env.sceneSeed = 1; env.os = in.sys.os; env.powerPlan = in.sys.powerPlan; env.cpu = in.sys.cpu; env.gpu = in.sys.gpu;
  env.gpuDriver = in.sys.gpuDriver; env.appHash = in.sys.appHash; env.monitorTopology = in.sys.monitorTopology;
  env.compositionSizes = std::to_string(in.width) + "x" + std::to_string(in.height);
  env.captureFormat = "synthetic features (no capture device in benchmark)"; env.encoder = "builtin-mjpeg"; env.bitrate = "n/a (MJPEG quality 80)";
  env.extra["note_ben03"] = "camera treatments only; GIF and video clip layers are not exercised by the headless benchmark";
  env.extra["note_ben06"] = "combined: two outputs (one independent) + text + program MJPEG encode to a disposable sink";
  env.extra["sinks"] = "disposable only: no socket, no DMX, no remote broadcast";
  return run.run(env);
}

}} // namespace ss::app
