// Owners: ui-live, ui-setup, ui-other (shared file, one line each). Add Tests/Ui<Area>SelfTests.cpp, declare its
// register function below and call it from registerUiSelfTests. Empty in the contract commit.
// Display-dependent tests run under xvfb-run (app-ui section 17).
#include "Tests/SelfTest.h"

namespace ss::app {

void registerUiLiveSelfTests(SelfTestRegistry&);
void registerUiSetupSelfTests(SelfTestRegistry&);

void registerUiSelfTests(SelfTestRegistry& reg) {
    registerUiLiveSelfTests(reg);   // ui-live
    registerUiSetupSelfTests(reg);  // ui-setup
    // ui-other: registerUiOtherSelfTests(reg);
}

}  // namespace ss::app
