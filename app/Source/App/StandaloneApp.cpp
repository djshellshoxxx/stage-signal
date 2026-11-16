// Custom standalone application (app-shell 6.1, decision D-SHELL-01). Owner: shell-integration.
// Needs JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1 (set in app/CMakeLists.txt): the stock StandalonePluginHolder is
// not built, so there is no input-mute switch and AppController owns the audio side.
//
// Verified to compile and link against JUCE 8.0.16-security (the Standalone wrapper then expects an external
// `juce_CreateApplication`, which START_JUCE_APPLICATION below defines). The fallback of app-shell 6.1 (stock
// holder with force-unmuted input) was therefore NOT needed.
//
// NOT done in this file yet (shell-integration follow-up): single-instance InterProcessLock, window state
// persistence, `--show <file>` / `--safe`, quit confirmation while recording/broadcasting, AudioProcessorPlayer
// wiring of the real device (engine-adapters-desktop supplies the IAudioSource that does it).
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>
#include "App/AppController.h"
#include "Engine/DesktopAdapters.h"
#include "Tests/SelfTest.h"
#include "UI/OperatorWindow.h"

#if JucePlugin_Build_Standalone

namespace {

class StageSignalApp final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "StageSignal"; }
    const juce::String getApplicationVersion() override { return "0.2.0"; }
    bool moreThanOneInstanceAllowed() override {
        // A self-test must run next to a normal instance; real instances are single (inter-process lock follow-up).
        return getCommandLineParameters().contains("--selftest");
    }

    void initialise(const juce::String& commandLine) override {
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        const int st = args.indexOf("--selftest");
        if (st >= 0) {
            const std::string which = (st + 1 < args.size() && !args[st + 1].startsWith("--")) ? args[st + 1].toStdString() : "smoke";
            setApplicationReturnValue(ss::app::runSelfTests(which));
            quit();
            return;
        }
        ss::app::AppConfig cfg;
        cfg.host = ss::app::HostKind::Standalone;
        controller_ = std::make_unique<ss::app::AppController>(ss::app::makeDesktopAdapterSet(), cfg);
        controller_->start();
        window_ = std::make_unique<ss::app::OperatorWindow>(controller_->ui(), [this] { systemRequestedQuit(); });
    }

    void shutdown() override {
        window_.reset();
        if (controller_) controller_->stop();
        controller_.reset();
    }

    void systemRequestedQuit() override { quit(); }     // recording/broadcast confirm dialog: follow-up
    void anotherInstanceStarted(const juce::String&) override { if (window_) window_->toFront(true); }

private:
    std::unique_ptr<ss::app::AppController> controller_;
    std::unique_ptr<ss::app::OperatorWindow> window_;
};

}  // namespace

START_JUCE_APPLICATION(StageSignalApp)

#endif
