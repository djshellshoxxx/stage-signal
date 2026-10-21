#include "Tests/SelfTest.h"
#include <cstdio>

namespace ss::app {

namespace {
std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o;
}
}  // namespace

int runSelfTests(const std::string& selector) {
    SelfTestRegistry reg;
    registerShellSelfTests(reg);
    registerAdaptersDesktopSelfTests(reg);
    registerMediaRecordSelfTests(reg);
    registerUiSelfTests(reg);
    const std::string want = selector.empty() ? "smoke" : selector;
    int failed = 0, ran = 0;
    for (const auto& t : reg.all()) {
        if (want != "all" && want != t.first) continue;
        ++ran;
        SelfTestResult r;
        try { r = t.second(); }
        catch (const std::exception& e) { r.ok = false; r.detail = std::string("exception: ") + e.what(); }
        catch (...) { r.ok = false; r.detail = "unknown exception"; }
        std::printf("{\"selftest\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}\n", t.first.c_str(), r.ok ? "true" : "false",
                    jsonEscape(r.detail).c_str());
        std::fflush(stdout);
        if (!r.ok) ++failed;
    }
    if (ran == 0) {
        std::printf("{\"selftest\":\"%s\",\"ok\":false,\"detail\":\"no such test\"}\n", jsonEscape(want).c_str());
        return 2;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace ss::app
