// Extension point for the real platform adapters. Owner: engine-adapters-desktop (media/record adapters are added
// by engine-media-record inside the same function through its own factory helper, see docs/specs/app-contract.md).
#pragma once
#include "Engine/AdapterSet.h"

namespace ss::app {

// Returns the adapters for the current desktop OS. CONTRACT COMMIT: not implemented, returns the fake set so the
// standalone app starts. When a real adapter lands, replace the matching member here and nothing else.
AdapterSet makeDesktopAdapterSet();

}  // namespace ss::app
