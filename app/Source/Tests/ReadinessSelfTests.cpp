#include <chrono>
#include <set>
#include <sstream>
#include <thread>
#include <fstream>
#include <filesystem>
#include <juce_core/juce_core.h>
#include "App/AppController.h"
#include "Engine/DesktopAdapters.h"
#include "Tests/SelfTest.h"

namespace ss::app {
namespace {
bool waitUntil(const std::function<bool()>& predicate, int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    while (std::chrono::steady_clock::now() < end);
    return predicate();
}
SelfTestResult blackoutStalledControl() {
    auto adapters = makeFakeAdapterSet(true);
    AppConfig config; config.headless = true;
    std::atomic<bool> entered{false}, released{false};
    AppController controller(adapters, config);
    struct ReleaseGuard { std::atomic<bool>& flag; ~ReleaseGuard() { flag.store(true); } } release{released};
    controller.registerActionHandler(ss::ActionId::NextScene, [&](const ss::Action&) {
        entered.store(true);
        while (!released.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ss::ActionResult result; result.accepted = true; return result;
    });
    if (!controller.start().ok()) return {false, "controller did not start"};
    if (!waitUntil([&] { return controller.ui().latestFrame(1) != nullptr; }, 10000)) return {false, "no initial frame"};
    ss::Action stall; stall.actionId = ss::id(ss::ActionId::NextScene); stall.source = ss::ActionSource::Ui;
    controller.ui().submitAction(stall);
    if (!waitUntil([&] { return entered.load(); }, 1000)) return {false, "CONTROL did not enter injected stall"};
    const auto beats = controller.heartbeats().beats(ThreadId::Control);
    const auto previous = controller.ui().latestFrame(1)->info.sequence;
    ss::Action blackout; blackout.actionId = ss::id(ss::ActionId::EmergencyBlackout); blackout.source = ss::ActionSource::Keyboard;
    if (!controller.ui().submitAction(blackout).result.latched) return {false, "emergency did not latch immediately"};
    const bool black = waitUntil([&] {
        auto frame = controller.ui().latestFrame(1);
        if (!frame || frame->info.sequence <= previous || !frame->info.blackout) return false;
        for (std::size_t i = 0; i < frame->pixels.rgba.size(); i += 4)
            if (frame->pixels.rgba[i] || frame->pixels.rgba[i + 1] || frame->pixels.rgba[i + 2]) return false;
        return true;
    }, 1000);
    if (!black || controller.heartbeats().beats(ThreadId::Control) != beats)
        return {false, "emergency output depended on CONTROL draining the queue"};
    released.store(true); controller.stop();
    return {true, "new all-black composed frame while CONTROL stayed blocked; latch bypasses stalled queue"};
}
SelfTestResult idleNetwork() {
#if JUCE_LINUX
    auto adapters = makeDesktopAdapterSet();
    AppConfig config; config.headless = true; config.startAudio = false;
    AppController controller(adapters, config);
    if (!controller.start().ok()) return {false, "native idle controller did not start"};
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::set<std::string> sockets;
    std::error_code error;
    const std::filesystem::directory_iterator descriptors("/proc/self/fd", error);
    if (error) return {false, "process descriptor table unavailable; idle-socket check did not execute"};
    for (const auto& entry : descriptors) {
        const auto target = std::filesystem::read_symlink(entry.path(), error).string();
        if (target.rfind("socket:[", 0) == 0 && target.back() == ']') sockets.insert(target.substr(8, target.size() - 9));
        error.clear();
    }
    for (const char* protocol : {"tcp", "tcp6", "udp", "udp6"}) {
        std::ifstream table(std::string("/proc/net/") + protocol);
        if (!table) return {false, "kernel network table unavailable; idle-socket check did not execute"};
        std::string line; std::getline(table, line);
        while (std::getline(table, line)) {
            std::istringstream fields(line); std::vector<std::string> words; std::string word;
            while (fields >> word) words.push_back(word);
            if (words.size() > 9 && sockets.count(words[9])) return {false, std::string("idle process owns an Internet socket: ") + protocol};
        }
    }
    controller.stop();
    return {true, "10-second native idle: no owned TCP/UDP IPv4/IPv6 sockets (local IPC is separate)"};
#else
    return {true, "SKIPPED: kernel TCP/UDP inode check is Linux-only; native OS network qualification is separate"};
#endif
}
}
void registerReadinessSelfTests(SelfTestRegistry& registry) {
    registry.add("shell: blackout bypasses stalled CONTROL", blackoutStalledControl);
    registry.add("shell: native idle has no Internet sockets", idleNetwork);
}
}
