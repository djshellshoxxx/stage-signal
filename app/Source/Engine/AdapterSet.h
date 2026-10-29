// Contract: the bundle of platform adapters injected into AppController. Owner: shell-integration.
// Every member is optional-by-fake: makeFakeAdapterSet() fills all of them so the controller runs headless.
// engine-adapters-desktop supplies makeDesktopAdapterSet() (Engine/DesktopAdapters.h) with real ones.
#pragma once
#include <memory>
#include "Engine/ICameraSource.h"
#include "Engine/IAudioSource.h"
#include "Engine/IDisplayProvider.h"
#include "Engine/IDurableFileSystem.h"
#include "Engine/IFileDialogs.h"
#include "Engine/ILogSink.h"
#include "Engine/IMidiPortProvider.h"
#include "Engine/IPacketSink.h"
#include "Engine/IPerfCounters.h"
#include "Engine/IProcessSupervisor.h"
#include "Engine/ISecretStoreAdapter.h"
#include "Engine/ISerialDmx.h"
#include "Engine/ITimestampMapper.h"
#include "Engine/IZipArchive.h"
#include "ss/base/time.hpp"

namespace ss::app {

struct AdapterSet {
    std::shared_ptr<ss::IClock> clock;
    std::shared_ptr<IAudioSource> audio;
    std::shared_ptr<ITimestampMapper> timestampMapper;
    std::shared_ptr<IMidiPortProvider> midi;
    std::shared_ptr<IPacketSink> packets;
    std::shared_ptr<ISerialDmx> serial;
    std::shared_ptr<IDisplayProvider> displays;
    std::shared_ptr<ICameraSource> camera;
    std::shared_ptr<IProcessSupervisor> processes;
    std::shared_ptr<ISecretStoreAdapter> secrets;
    std::shared_ptr<IDurableFileSystem> files;
    std::shared_ptr<IZipArchive> zip;
    std::shared_ptr<IPerfCounters> perf;
    std::shared_ptr<IFileDialogs> dialogs;
    std::shared_ptr<ILogSink> log;
};

// All fakes. `audio` is a SineAudioSource when withSine, otherwise NoAudioSource.
AdapterSet makeFakeAdapterSet(bool withSine = true);

}  // namespace ss::app
