// App-level benchmark scenarios (spec §7 BEN-01..06) run through perf/BenchmarkRunner on DISPOSABLE sinks only:
// no real socket, no broadcast, no DMX transmission, no live show. Uses real Renderer/encoders with synthetic features.
#pragma once
#include "core/app/app_types.h"
#include "perf/benchmark.h"
#include <atomic>
#include <set>

namespace ss { namespace app {

struct AppBenchInputs {
  IClock* clock = nullptr;                  // pacing clock (system clock, or a FakeClock for deterministic tests)
  FakeClock* fake = nullptr;                // non-null: the scenario steps this clock instead of sleeping
  SystemInfo sys;
  ICpuCounters* cpu = nullptr; IMemCounters* mem = nullptr;
  const EncoderRegistry* encoders = nullptr;
  ITextRasterizer* text = nullptr;
  const std::atomic<bool>* cancel = nullptr;
  int width = 800, height = 600;
  std::set<BenchId> scenarios = {BenchId::BEN01};
  double durationScale = 1.0;
};
BenchResult runAppBenchmark(const AppBenchInputs& in);

}} // namespace ss::app
