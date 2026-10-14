#include "types.h"
namespace ss {
const char* errName(Err e) {
  static const char* n[] = {"Ok","InvalidArgument","UnsupportedCapability","PermissionDenied","DeviceLost",
    "TimestampInvalid","ResourceExhausted","DecodeFailure","EncoderUnavailable","DiskFull",
    "TransportFailure","SchemaUnsupported","Cancelled","InternalFault"};
  return (size_t)e < sizeof(n)/sizeof(*n) ? n[(size_t)e] : "?";
}
namespace { struct SysClock : IClock {
  Ns nowNs() const override { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
  int64_t wallUnixMs() const override { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}; }
IClock& systemClock() { static SysClock c; return c; }
}
