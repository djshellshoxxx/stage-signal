// Contract: standalone vs CLAP differences as a small strategy (app-shell 6, 7; APP-SH-14, DEC-11).
// Owner: shell-integration. Format differences live HERE, never as forks in the processor or controller.
#pragma once

namespace ss::app {

enum class HostKind : unsigned char { Standalone, Clap };

class HostMode {
public:
    virtual ~HostMode() = default;
    virtual HostKind kind() const = 0;
    virtual const char* name() const = 0;
    // Standalone: processor output is silence (monitoring off, spec 12). CLAP: leave the host buffer untouched.
    virtual bool clearsProcessorOutput() const = 0;
    // CLAP: main bus is bit-exact passthrough; dynamics apply only to the record/broadcast tap.
    virtual bool passesThroughMainBus() const = 0;
    // Standalone: AppController owns the AudioDeviceManager. CLAP: the host owns the device.
    virtual bool ownsAudioDevice() const = 0;
    // CLAP never opens audience windows at load; standalone opens only routes whose stored display is present.
    virtual bool opensAudienceWindowsAtStartup() const = 0;
    // CLAP: the editor may close while the plugin keeps running; standalone owns an OperatorWindow.
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

class ClapHostMode final : public HostMode {
public:
    HostKind kind() const override { return HostKind::Clap; }
    const char* name() const override { return "CLAP"; }
    bool clearsProcessorOutput() const override { return false; }
    bool passesThroughMainBus() const override { return true; }
    bool ownsAudioDevice() const override { return false; }
    bool opensAudienceWindowsAtStartup() const override { return false; }
    bool editorIsEmbedded() const override { return true; }
};

inline const HostMode& hostModeFor(HostKind k) {
    static const StandaloneHostMode standalone;
    static const ClapHostMode clap;
    return k == HostKind::Clap ? static_cast<const HostMode&>(clap) : static_cast<const HostMode&>(standalone);
}

}  // namespace ss::app
