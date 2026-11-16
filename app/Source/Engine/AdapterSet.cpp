#include "Engine/AdapterSet.h"
#include "Engine/Fakes.h"

namespace ss::app {

AdapterSet makeFakeAdapterSet(bool withSine) {
    AdapterSet s;
    s.clock = std::make_shared<ss::SystemClock>();
    if (withSine) s.audio = std::make_shared<SineAudioSource>();
    else s.audio = std::make_shared<NoAudioSource>();
    s.timestampMapper = std::make_shared<FixedLatencyTimestampMapper>();
    s.midi = std::make_shared<FakeMidiPortProvider>();
    s.packets = std::make_shared<RecordingPacketSink>();
    s.serial = std::make_shared<NullSerialDmx>();
    s.displays = std::make_shared<FakeDisplayProvider>();
    s.camera = std::make_shared<NullCameraSource>();
    s.processes = std::make_shared<FakeProcessSupervisor>();
    s.secrets = std::make_shared<MemorySecretStoreAdapter>();
    s.files = std::make_shared<StdDurableFileSystem>();
    s.zip = std::make_shared<UnsupportedZipArchive>();
    s.perf = std::make_shared<FixedPerfCounters>();
    s.dialogs = std::make_shared<ScriptedFileDialogs>();
    s.log = std::make_shared<NullLogSink>();
    return s;
}

}  // namespace ss::app
