#include "doctest/doctest.h"
#include "ss/base/actions.hpp"
#include "ss/model/show_model.hpp"
using namespace ss;
TEST_CASE("model defaults are sane and action ids are stable") {
    model::ShowDocument d;
    CHECK(d.outputs.size() == 1);
    CHECK(d.schemaMajor == model::kSchemaMajor);
    CHECK(id(ActionId::EmergencyBlackout) == 22);   // stored in show files: never renumber
    CHECK(id(ActionId::Undo) == 100);
    CHECK(isSafetyCritical(ActionId::StopRecording));
    CHECK_FALSE(isSafetyCritical(ActionId::NextScene));
    model::SceneParams a, b;
    CHECK(a == b);
    b.hue = 0.5f;
    CHECK_FALSE(a == b);
}
