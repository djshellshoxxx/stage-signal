#include "ss/render/scene.hpp"
#include "scenes.hpp"

namespace ss::render {

std::unique_ptr<SceneRenderer> makeScene(model::SceneKind kind) {
    using K = model::SceneKind;
    switch (kind) {
        case K::SpectrumBars: return detail::makeSpectrumBars();
        case K::MirrorBars: return detail::makeMirrorBars();
        case K::WaveformRibbon: return detail::makeWaveformRibbon();
        case K::GeometricOrbit: return detail::makeGeometricOrbit();
        case K::RadialPulse: return detail::makeRadialPulse();
        case K::ParticleField: return detail::makeParticleField();
        case K::NeonGrid: return detail::makeNeonGrid();
        case K::Tunnel: return detail::makeTunnel();
        case K::ColorField: return detail::makeColorField();
        case K::Starburst: return detail::makeStarburst();
        case K::PlasmaLite: return detail::makePlasmaLite();
        case K::LissajousScope: return detail::makeLissajousScope();
        default: return nullptr;
    }
}

}  // namespace ss::render
