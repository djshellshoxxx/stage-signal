#include "Engine/DesktopAdapters.h"

namespace ss::app {

AdapterSet makeDesktopAdapterSet() {
    // STUB (contract commit): no real adapter exists yet. Audio is "No audio device" so the standalone app never
    // pretends to capture; every other member is the fake/null from makeFakeAdapterSet().
    return makeFakeAdapterSet(/*withSine=*/false);
}

}  // namespace ss::app
