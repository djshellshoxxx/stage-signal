// Built-in sample show (app-shell 5 step 4 fallback when no show loads). Owner: shell-integration.
#pragma once
#include "ss/model/show_model.hpp"

namespace ss::app {

// Six non-media scenes across energy tiers, one 800x600 output, Auto mode, strobing disabled (safe default).
ss::model::ShowDocument makeBuiltInSampleShow();

}  // namespace ss::app
