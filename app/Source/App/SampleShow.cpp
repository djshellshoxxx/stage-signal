#include "App/SampleShow.h"

namespace ss::app {

ss::model::ShowDocument makeBuiltInSampleShow() {
    using namespace ss::model;
    ShowDocument d;
    d.name = "Sample show";
    d.uuid = "00000000-0000-4000-8000-000000000001";
    struct S { const char* name; SceneKind kind; EnergyTier tier; };
    const S table[] = {
        {"Spectrum", SceneKind::SpectrumBars, EnergyTier::Low},
        {"Waveform", SceneKind::WaveformRibbon, EnergyTier::Low},
        {"Radial", SceneKind::RadialPulse, EnergyTier::Medium},
        {"Neon grid", SceneKind::NeonGrid, EnergyTier::Medium},
        {"Tunnel", SceneKind::Tunnel, EnergyTier::High},
        {"Starburst", SceneKind::Starburst, EnergyTier::Drop},
    };
    std::uint32_t id = 1;
    for (const auto& s : table) {
        SceneDef sc;
        sc.id = id; sc.name = s.name; sc.kind = s.kind; sc.tier = s.tier; sc.seed = 1000 + id;
        d.scenes.push_back(sc);
        ++id;
    }
    d.outputs = {OutputDef{}};      // Main, audience, 800x600
    d.mode = ss::ShowMode::Auto;
    d.seed = 42;
    d.strobingDisabled = true;
    return d;
}

}  // namespace ss::app
