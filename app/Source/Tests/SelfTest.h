// Headless self-test entry (`StageSignal --selftest [name|all]`), app-shell section 13. Owner: shell-integration.
// Tests construct an AppController with FAKE adapters, never touch hardware, and return a result; the runner
// prints one JSON line per test and the process exit code is 0 only if every selected test passed.
//
// Adding tests (no edits to this file): add cases in YOUR area's file and register them in YOUR register function:
//   Tests/ShellSelfTests.cpp           shell-integration       registerShellSelfTests
//   Tests/AdaptersDesktopSelfTests.cpp engine-adapters-desktop registerAdaptersDesktopSelfTests
//   Tests/MediaRecordSelfTests.cpp     engine-media-record     registerMediaRecordSelfTests
//   Tests/UiSelfTests.cpp              ui-live, ui-setup, ui-other (add subfiles Tests/Ui<Area>SelfTests.cpp and call
//                                      them from registerUiSelfTests; that file is shared by the three UI workers, so
//                                      keep edits to one line each)
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace ss::app {

struct SelfTestResult {
    bool ok = true;
    std::string detail;                  // first failure or a short summary; sanitised, no paths
};

class SelfTestRegistry {
public:
    using Fn = std::function<SelfTestResult()>;
    void add(const std::string& name, Fn fn) { tests_.push_back({name, std::move(fn)}); }
    const std::vector<std::pair<std::string, Fn>>& all() const { return tests_; }
private:
    std::vector<std::pair<std::string, Fn>> tests_;
};

void registerShellSelfTests(SelfTestRegistry&);
void registerAdaptersDesktopSelfTests(SelfTestRegistry&);
void registerMediaRecordSelfTests(SelfTestRegistry&);
void registerUiSelfTests(SelfTestRegistry&);

// "smoke" is the default. "all" runs every registered test. Returns the process exit code (0 = pass).
int runSelfTests(const std::string& selector);

}  // namespace ss::app
