// Native child-process supervisor (app-adapters 8.2, limitation P-07). Owner: engine-media-record.
// argv vector only (no shell), scrubbed environment, separate stdout/stderr capture, bounded non-blocking stdin and
// an optional second input pipe (fd 3, POSIX only). Windows: CreateProcess with anonymous pipes (stdin/out/err only).
#pragma once
#include <memory>
#include "Engine/IProcessSupervisor.h"

namespace ss::app {

std::shared_ptr<IProcessSupervisor> makeNativeProcessSupervisor();

// Result of a bounded synchronous run (used for ffmpeg discovery). `ok` = started and exited 0 before the timeout.
struct CapturedRun { bool started = false, exited = false, timedOut = false; int code = -1; std::string out, err; };
CapturedRun runCaptured(IProcessSupervisor& sup, const std::vector<std::string>& argv, int timeoutMs);

}  // namespace ss::app
