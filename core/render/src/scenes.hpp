// Internal: per-scene factory functions (one per scenes_*.cpp translation unit).
#pragma once
#include <memory>
#include "scene_common.hpp"

namespace ss::render::detail {
std::unique_ptr<SceneRenderer> makeSpectrumBars();
std::unique_ptr<SceneRenderer> makeMirrorBars();
std::unique_ptr<SceneRenderer> makeWaveformRibbon();
std::unique_ptr<SceneRenderer> makeGeometricOrbit();
std::unique_ptr<SceneRenderer> makeRadialPulse();
std::unique_ptr<SceneRenderer> makeParticleField();
std::unique_ptr<SceneRenderer> makeNeonGrid();
std::unique_ptr<SceneRenderer> makeTunnel();
std::unique_ptr<SceneRenderer> makeColorField();
std::unique_ptr<SceneRenderer> makeStarburst();
std::unique_ptr<SceneRenderer> makePlasmaLite();
std::unique_ptr<SceneRenderer> makeLissajousScope();
}  // namespace ss::render::detail
