// Custom standalone application (app-shell 6.1, decision D-SHELL-01). Owner: shell-integration.
// Needs JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1 (set in app/CMakeLists.txt): the stock StandalonePluginHolder is
// not built, so there is no input-mute switch and AppController owns the audio side.
//
// Verified to compile and link against JUCE 8.0.16-security (the Standalone wrapper then expects an external
// `juce_CreateApplication`, which START_JUCE_APPLICATION below defines). The fallback of app-shell 6.1 (stock
// holder with force-unmuted input) was therefore NOT needed.
//
// Done here: single-instance InterProcessLock (on top of JUCE's own pipe hand-off), `--show <file>` / `--safe`, data
// folder for settings/running.lock/autosave, quit confirmation (unsaved show, recording, broadcast).
// NOT done: window position persistence per display identity, AudioProcessorPlayer wiring of the real device
// (the IAudioSource feeds AudioIngress directly).
#include <cstdio>
#include <cstdlib>
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
        // A self-test must run next to a normal instance; real instances are single.
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
        // One instance per user: the lock protects the data folder (running.lock, autosave) even if the pipe hand-off fails.
        instanceLock_ = std::make_unique<juce::InterProcessLock>("StageSignal-instance");
        if (!instanceLock_->enter(0)) {
            std::fprintf(stderr, "StageSignal is already running.\n");
            setApplicationReturnValue(3);
            quit();
            return;
        }
        ss::app::AppConfig cfg;
        cfg.host = ss::app::HostKind::Standalone;
        cfg.dataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("StageSignal").getFullPathName().toStdString();
        const int sh = args.indexOf("--show");
        if (sh >= 0 && sh + 1 < args.size()) cfg.showPath = juce::File::getCurrentWorkingDirectory().getChildFile(args[sh + 1].unquoted()).getFullPathName().toStdString();
        cfg.safeMode = args.contains("--safe");
        if (const char* d = std::getenv("SS_DATA_DIR")) cfg.dataDir = d;      // tests and portable installs
        controller_ = std::make_unique<ss::app::AppController>(ss::app::makeDesktopAdapterSet(), cfg);
        controller_->start();
        window_ = std::make_unique<ss::app::OperatorWindow>(controller_->ui(), [this] { systemRequestedQuit(); });
    }

    void shutdown() override {
        window_.reset();
        if (controller_) controller_->stop();
        controller_.reset();
    }

    void systemRequestedQuit() override {
        if (!controller_ || quitAsked_) { quit(); return; }
        const bool media = controller_->mediaActive(), unsaved = controller_->hasUnsavedWork();
        if (!media && !unsaved) { quit(); return; }
        quitAsked_ = true;
        juce::String msg;
        if (media) msg << "Recording or broadcast is running. Quitting stops it.\n";
        if (unsaved) msg << "The show has changes that are not saved.";
        auto opts = juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::WarningIcon).withTitle("Quit StageSignal?")
                        .withMessage(msg).withButton(unsaved ? "Save and quit" : "Quit").withButton(unsaved ? "Quit without saving" : "Cancel");
        if (unsaved) opts = opts.withButton("Cancel");
        juce::AlertWindow::showAsync(opts, [this, unsaved](int r) {
            quitAsked_ = false;
            // Button order: 1 first, 2 second, 0 third (cancel).
            if (unsaved) {
                if (r == 1) { if (controller_->saveShowNow().ok()) { controller_->discardUnsavedWork(); quit(); } }
                else if (r == 2) { controller_->discardUnsavedWork(); quit(); }
            } else if (r == 1) quit();
        });
    }
    void anotherInstanceStarted(const juce::String&) override { if (window_) window_->toFront(true); }

private:
    std::unique_ptr<juce::InterProcessLock> instanceLock_;
    bool quitAsked_ = false;
    std::unique_ptr<ss::app::AppController> controller_;
    std::unique_ptr<ss::app::OperatorWindow> window_;
};

}  // namespace

START_JUCE_APPLICATION(StageSignalApp)

#endif
