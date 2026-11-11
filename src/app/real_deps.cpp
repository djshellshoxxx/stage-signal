#include "app/real_deps.h"
#include "platform/windows/platform.h"
#include "perf/metrics.h"

namespace ss { namespace app {
namespace {
// ICpuCounters / IMemCounters over win::readSysCounters (deltas between consecutive samples).
class WinCpu : public ICpuCounters {
 public:
  CpuSample sample() override {
    win::SysCounters c; CpuSample out; if (!win::readSysCounters(c) || !c.valid) return out;
    if (have_ && c.wallNs > prev_.wallNs) {
      double wall100 = (double)(c.wallNs - prev_.wallNs) / 100.0; unsigned n = c.logicalCores ? c.logicalCores : 1;
      double proc = (double)((c.procKernel100ns + c.procUser100ns) - (prev_.procKernel100ns + prev_.procUser100ns));
      double sysTot = (double)((c.sysKernel100ns + c.sysUser100ns) - (prev_.sysKernel100ns + prev_.sysUser100ns)), sysIdle = (double)(c.sysIdle100ns - prev_.sysIdle100ns);
      out.available = true; out.logicalCpus = n; out.processPct = clamp(proc / (wall100 * n) * 100.0); out.systemPct = sysTot > 0 ? clamp((sysTot - sysIdle) / sysTot * 100.0) : 0;
      if (c.perCoreAvailable && prev_.perCoreAvailable && c.coreTotal100ns.size() == prev_.coreTotal100ns.size()) {
        double worst = 0; for (size_t i = 0; i < c.coreTotal100ns.size(); ++i) { double t = (double)(c.coreTotal100ns[i] - prev_.coreTotal100ns[i]), id = (double)(c.coreIdle100ns[i] - prev_.coreIdle100ns[i]); if (t > 0) worst = std::max(worst, (t - id) / t * 100.0); }
        out.perCoreAvailable = true; out.busiestCorePct = clamp(worst);
      }
    }
    prev_ = c; have_ = true; return out;
  }
 private:
  static double clamp(double v) { return v < 0 ? 0 : v > 100 ? 100 : v; }
  win::SysCounters prev_; bool have_ = false;
};
class WinMem : public IMemCounters {
 public:
  MemSample sample() override {
    MemSample m; win::SysCounters c; if (!win::readSysCounters(c) || !c.memoryValid) return m;
    m.workingSetBytes = Maybe<uint64_t>(c.workingSetBytes); m.privateBytes = Maybe<uint64_t>(c.privateBytes); m.availPhysBytes = Maybe<uint64_t>(c.physAvailBytes); return m;
  }
};
} // namespace

struct RealDeps::Impl {
  std::unique_ptr<IAudioCapture> audio; std::unique_ptr<IDisplayProvider> displays; std::unique_ptr<ICamera> camera; std::unique_ptr<ITextRasterizer> text;
  std::unique_ptr<IMidiPort> midi; std::unique_ptr<IDatagramSink> sink; std::unique_ptr<ISecretStore> secrets; std::unique_ptr<IDiskInfo> disk; WinCpu cpu; WinMem mem;
};

RealDeps::RealDeps(const RealDepsOptions& o) : p_(new Impl()) {
  enc_ = EncoderRegistry::withBuiltins(); win::registerWindowsEncoders(enc_);            // H.264 (Media Foundation) unqualified until benchmarked
  tf_.setByteStreamFactory([] { return win::makeTcpStream(); });                         // RTMP / RTMPS (Schannel, verified chain + host name)
  tf_.setUdpSocketFactory([] { return win::makeBroadcastUdpSocket(); });                 // RTP
  tf_.setClock(&systemClock());                                                          // SRT stays UnsupportedCapability (no stack bundled)
  p_->displays = win::makeDisplayProvider(); p_->text = win::makeTextRasterizer(); p_->disk = win::makeDiskInfo();
  if (o.devices) { p_->audio = win::makeAudioCapture(); p_->camera = win::makeCamera(); p_->midi = win::makeMidiPort(); p_->sink = win::makeDatagramSink(); p_->secrets = win::makeSecretStore(); }
  d_.audio = p_->audio.get(); d_.displays = p_->displays.get(); d_.camera = p_->camera.get(); d_.text = p_->text.get();
  d_.midiIn = p_->midi.get(); d_.midiOut = p_->midi.get(); d_.lightingSink = p_->sink.get(); d_.secrets = p_->secrets.get();
  if (o.devices && win::mediaFoundationAvailable()) d_.makeVideoDecoder = [] { return win::makeVideoDecoder(); };
  d_.disk = p_->disk.get(); d_.encoders = &enc_; d_.transports = &tf_; d_.cpu = &p_->cpu; d_.mem = &p_->mem;
  d_.sys.os = win::windowsVersionString(); d_.sys.powerPlan = win::powerPlanName(); d_.sys.cpu = win::cpuModelString(); d_.sys.appHash = "0.1.0";
}
RealDeps::~RealDeps() = default;

std::string RealDeps::protocolIssue(BroadcastProtocol p) const {
  switch (p) {
    case BroadcastProtocol::RTMPS: return win::schannelTlsAvailable() ? std::string() : "RTMPS needs Schannel TLS 1.1/1.2 with certificate validation, which this Windows does not provide; it will not connect unverified.";
    case BroadcastProtocol::RTMP: case BroadcastProtocol::RTP: return std::string();
    case BroadcastProtocol::SRT: return TransportFactory::unsupportedReason(p);
  }
  return "unsupported";
}
}} // namespace ss::app
