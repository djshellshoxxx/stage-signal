// Builds the real AppDeps from the Windows adapter factories (platform.h) and owns every adapter object.
// Destroy the AppController BEFORE this object (AppDeps holds borrowed pointers).
#pragma once
#include "core/app/controller.h"
#include "broadcast/transport.h"
#include "recording/video_encoder.h"
#include <memory>

namespace ss { namespace app {

struct RealDepsOptions {
  bool devices = true;                  // false: no audio/camera/MIDI/secret store/lighting sink (headless self-test)
};

class RealDeps {
 public:
  explicit RealDeps(const RealDepsOptions& o = RealDepsOptions());
  ~RealDeps();
  AppDeps& deps() { return d_; }
  const EncoderRegistry& encoders() const { return enc_; }
  std::string protocolIssue(BroadcastProtocol p) const;   // "" = usable, otherwise why not (shown in the Broadcast tab)
 private:
  struct Impl; std::unique_ptr<Impl> p_;
  EncoderRegistry enc_; TransportFactory tf_; AppDeps d_;
};

}} // namespace ss::app
