// Contract: standalone vs hosted-plugin differences as a small strategy (app-shell 6, 7; APP-SH-14, DEC-11).
// Owner: shell-integration. Format differences live HERE, never as forks in the processor or controller.
#pragma once

namespace ss::app {

enum class HostKind : unsigned char { Standalone, Clap, Vst3 };

class HostMode {
public:
    virtual ~HostMode() = default;
    virtual HostKind kind() const = 0;
    virtual const char* name() const = 0;
    // Standalone: processor output is silence (monitoring off, spec 12). Plugins: leave the host buffer untouched.
    virtual bool clearsProcessorOutput() const = 0;
    // Plugins: main bus is bit-exact passthrough; dynamics apply only to the record/broadcast tap.
    virtual bool passesThroughMainBus() const = 0;
    // Standalone: AppController owns the AudioDeviceManager. Plugins: the host owns the device.
    virtual bool ownsAudioDevice() const = 0;
    // Plugins never open audience windows at load; standalone opens only routes whose stored display is present.
    virtual bool opensAudienceWindowsAtStartup() const = 0;
    // Plugins: the editor may close while the plugin keeps running; standalone owns an OperatorWindow.
    virtual bool editorIsEmbedded() const = 0;
};

class StandaloneHostMode final : public HostMode {
public:
    HostKind kind() const override { return HostKind::Standalone; }
    const char* name() const override { return "Standalone"; }
    bool clearsProcessorOutput() const override { return true; }
    bool passesThroughMainBus() const override { return false; }
    bool ownsAudioDevice() const override { return true; }
    bool opensAudienceWindowsAtStartup() const override { return true; }
    bool editorIsEmbedded() const override { return false; }
};

class PluginHostMode final : public HostMode {
public:
    explicit PluginHostMode(HostKind kind) : kind_(kind) {}
    HostKind kind() const override { return kind_; }
    const char* name() const override { return kind_ == HostKind::Vst3 ? "VST3" : "CLAP"; }
    bool clearsProcessorOutput() const override { return false; }
    bool passesThroughMainBus() const override { return true; }
    bool ownsAudioDevice() const override { return false; }
    bool opensAudienceWindowsAtStartup() const override { return false; }
    bool editorIsEmbedded() const override { return true; }

private:
    HostKind kind_;
};

inline const HostMode& hostModeFor(HostKind k) {
    static const StandaloneHostMode standalone;
    static const PluginHostMode clap(HostKind::Clap), vst3(HostKind::Vst3);
    if (k == HostKind::Vst3) return vst3;
    return k == HostKind::Clap ? static_cast<const HostMode&>(clap) : static_cast<const HostMode&>(standalone);
}

}  // namespace ss::app
