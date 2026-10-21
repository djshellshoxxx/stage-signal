#include "Engine/DesktopAdapters.h"
#include <cstdlib>
#include "Engine/Desktop/DesktopSystem.h"
#include "Engine/Desktop/JuceAudioSource.h"
#include "Engine/Desktop/JuceMidiProvider.h"
#include "Engine/Desktop/TimestampMapperLinear.h"
#include "Engine/Desktop/UdpPacketSink.h"
#include "Engine/Media/NativeProcess.h"

namespace ss::app {

AdapterSet makeDesktopAdapterSet() {
    // Start from the fakes so any member without a desktop implementation (camera, process supervisor, serial DMX,
    // owned by other workers or optional) keeps its honest fake/null. SS_FAKE_ADAPTERS=1 keeps the pure fake set.
    AdapterSet s = makeFakeAdapterSet(/*withSine=*/false);
    if (const char* f = std::getenv("SS_FAKE_ADAPTERS"); f && *f == '1') return s;

    auto mapper = std::make_shared<LinearFitTimestampMapper>();
    s.timestampMapper = mapper;
    // With no input device the real source reports Unavailable ("No audio device") exactly like NoAudioSource.
    s.audio = std::make_shared<JuceAudioSource>(s.clock, mapper);
    s.midi = std::make_shared<JuceMidiProvider>(s.clock);
    s.packets = std::make_shared<UdpPacketSink>();
    s.displays = std::make_shared<JuceDisplayProvider>();
    s.secrets = std::make_shared<OsSecretStore>();
    s.files = std::make_shared<FsyncDurableFileSystem>();
    s.zip = std::make_shared<SafeZipArchive>();
    s.perf = std::make_shared<ProcessPerfCounters>(s.clock);
    s.dialogs = std::make_shared<JuceFileDialogs>();
    s.log = std::make_shared<RingLogSink>();
    s.processes = makeNativeProcessSupervisor();      // ffmpeg and other children: argv only, scrubbed environment
    // s.serial stays NullSerialDmx: reports Unavailable ("no serial adapter in this build"), see app-adapters 7.
    return s;
}

}  // namespace ss::app
