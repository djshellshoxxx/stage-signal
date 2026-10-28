// Native camera capture on JUCE-supported desktop OSes. Opening never publishes a frame.
#pragma once
#include <memory>
#include "Engine/ICameraSource.h"

namespace ss::app {
std::shared_ptr<ICameraSource> makeJuceCameraSource(std::shared_ptr<ss::IClock> clock);
}
