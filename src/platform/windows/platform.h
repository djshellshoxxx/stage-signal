// StageSignal Windows adapters: single public header (no Windows headers leak out of this directory).
// Implements the ports in core/ports.h. All factories are in namespace ss::win. Baseline: XP/Vista-loadable
// binary (newer APIs resolved at runtime via GetProcAddress/LoadLibrary and reported as capability errors).
#pragma once
#include "core/ports.h"
#include "broadcast/rtmp.h"
#include "broadcast/rtp.h"
#include "recording/io.h"
#include "recording/video_encoder.h"
#include "recording/audio_encoder.h"
#include "lighting/usb_dmx.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace ss { namespace win {

// ---- Audio (REQ-A01/A02, IF-01) -----------------------------------------------------------
// Backend is chosen EXPLICITLY. Auto = WASAPI when the MMDevice API is present on this OS at construction time,
// otherwise WinMM; it never switches backend or device after construction. Device ids are backend-prefixed
// ("{0.0.1...}" raw endpoint ids for WASAPI, "winmm:..." for WinMM) so a saved id never matches the wrong backend.
enum class AudioBackend : uint8_t { Auto, Wasapi, WinMM };
std::unique_ptr<IAudioCapture> makeAudioCapture(AudioBackend b = AudioBackend::Auto);
bool wasapiAvailable();                       // true if IMMDeviceEnumerator can be created (Vista+)

// ---- Displays (MOD-05, REQ-D01..D06) ------------------------------------------------------
// Change callback fires on an internal message-window thread (debounced ~250 ms): set a flag / post to a queue only.
std::unique_ptr<IDisplayProvider> makeDisplayProvider();

// ---- Camera / video (Media Foundation, loaded at runtime; absent on XP => UnsupportedCapability) ----
std::unique_ptr<ICamera> makeCamera();
std::unique_ptr<IVideoDecoder> makeVideoDecoder();
bool mediaFoundationAvailable();

// ---- Text ---------------------------------------------------------------------------------
std::unique_ptr<ITextRasterizer> makeTextRasterizer();

// ---- MIDI (WinMM) -------------------------------------------------------------------------
std::unique_ptr<IMidiPort> makeMidiPort();

// ---- Network ------------------------------------------------------------------------------
// IPv4 UDP lighting sink. nicAddr = dotted IPv4 of the selected NIC ("" = OS default). Broadcast only if allowBroadcast.
std::unique_ptr<IDatagramSink> makeDatagramSink();

struct NicInfo { std::string name; std::string ipv4; std::string netmask; bool up = false; };
std::vector<NicInfo> listNics();              // IPv4 unicast addresses of operational adapters

// Thin UDP socket (own interface on purpose; the lead adapts to broadcast/transport.h).
struct IUdpSocketW {
  virtual ~IUdpSocketW() = default;
  // localAddr "" = any. port 0 = ephemeral. reuse = SO_REUSEADDR. broadcast = SO_BROADCAST (opt-in).
  virtual Status bind(const std::string& localAddr, uint16_t port, bool reuse, bool broadcast) = 0;
  virtual Status joinMulticast(const std::string& group, const std::string& nicAddr) = 0;
  virtual Status sendTo(const std::string& host, uint16_t port, const uint8_t* d, size_t n) = 0;
  // Returns bytes received (>0), 0 on timeout, <0 on error. fromHost/fromPort may be null.
  virtual int recvFrom(uint8_t* buf, size_t cap, int timeoutMs, std::string* fromHost, uint16_t* fromPort) = 0;
  virtual uint16_t localPort() const = 0;
  virtual void close() = 0;
  virtual Status setTtl(int ttl) { (void)ttl; return Status::Ok(); }   // IP_TTL + IP_MULTICAST_TTL (additive; default no-op)
};
std::unique_ptr<IUdpSocketW> makeUdpSocket();

// Adapts IUdpSocketW to the broadcast IUdpSocket (RTP sender). open(nic, ttl) binds an ephemeral port on nic and sets the TTL.
std::unique_ptr<IUdpSocket> adaptUdpSocket(std::unique_ptr<IUdpSocketW> w);
std::unique_ptr<IUdpSocket> makeBroadcastUdpSocket();      // = adaptUdpSocket(makeUdpSocket())

// TCP byte stream for the portable RTMP/RTMPS client (broadcast/rtmp.h). Winsock; TLS via Schannel (secur32/crypt32 bound at run
// time, so the exe still starts on old Windows). TLS = TLS 1.2/1.1 client with SNI; the server chain AND host name are validated
// with CertGetCertificateChain + CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL); there is NO override. Revocation is not
// checked. supportsTlsVerification() is false when Schannel/crypt32 entry points are missing => RTMPS is refused by the factory.
// Register: factory.setByteStreamFactory([]{ return ss::win::makeTcpStream(); });
std::unique_ptr<IByteStream> makeTcpStream();
bool schannelTlsAvailable();

// ---- Recording disk space (IDiskInfo, spec §12) --------------------------------------------
// GetDiskFreeSpaceExW on the nearest existing parent of the path (per-user quota aware).
std::unique_ptr<IDiskInfo> makeDiskInfo();

// ---- USB DMX serial (REQ-L06). NOT hardware-qualified: no adapter is listed as supported until it has recorded results. ----
struct SerialPortInfo { std::string portName; std::string friendlyName; };   // "COM3", "USB Serial Port (COM3)"
std::vector<SerialPortInfo> listSerialPorts();                                // registry HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM (device object name is the label)
// CreateFile("\\.\COMn", overlapped), 250000 baud 8N2 (DMX512 line settings), write with 1 s timeout.
std::unique_ptr<IUsbSerialLink> makeUsbSerialLink();
// Named adapters whose protocol is implemented (label: "protocol implemented, not hardware-qualified"). Empty qualified list stays empty.
std::vector<UsbAdapterInfo> namedUsbAdapters();

// ---- Media Foundation encoders (loaded at run time; absent => honest EncoderUnavailable) -------------------------------------
// Registers 'h264-mf' (CLSID_CMSH264EncoderMFT, software, baseline, CBR, 2 s keyframes, low latency; qualified=false) into the registry.
// RGB32 frames are converted to NV12 in scalar code. Packets are Annex B; codecPrivate() = avcC.
void registerWindowsEncoders(EncoderRegistry& r);
// AAC-LC (CLSID_AACMFTEncoder, raw payload) producing AudioSpecificConfig for RTMP; nullptr-equivalent status via open().
std::unique_ptr<IAudioEncoder> makeAacEncoder();
EncoderAvailability h264EncoderAvailability();                // probes by creating the MFT once
EncoderAvailability aacEncoderAvailability();

// ---- Secrets (REQ-B03) --------------------------------------------------------------------
// DPAPI per-user; files under %LOCALAPPDATA%\StageSignal\secrets\<hash(ref)>.bin. Never logs/echoes secrets.
std::unique_ptr<ISecretStore> makeSecretStore();

// ---- System info / counters for the perf agent ---------------------------------------------
std::string windowsVersionString();           // e.g. "Windows 6.1.7601 SP1 (x86 on x64/WOW64)"
std::string powerPlanName();                  // "Balanced" etc., "unavailable" if unknown
std::string cpuModelString();                 // registry ProcessorNameString (+~MHz) or CPUID brand

struct SysCounters {
  bool valid = false;                         // process + system times read
  uint64_t wallNs = 0;                        // QPC monotonic at sample time
  uint64_t procKernel100ns = 0, procUser100ns = 0;   // GetProcessTimes (cumulative)
  uint64_t sysIdle100ns = 0, sysKernel100ns = 0, sysUser100ns = 0;  // GetSystemTimes (kernel includes idle)
  uint32_t logicalCores = 1;
  bool perCoreAvailable = false;              // NtQuerySystemInformation(SystemProcessorPerformanceInformation)
  std::vector<uint64_t> coreIdle100ns, coreTotal100ns;   // per-core cumulative (kernel incl. idle + user)
  bool memoryValid = false;
  uint64_t workingSetBytes = 0, peakWorkingSetBytes = 0, privateBytes = 0;  // GetProcessMemoryInfo
  uint64_t physTotalBytes = 0, physAvailBytes = 0;     // GlobalMemoryStatus(Ex)
};
bool readSysCounters(SysCounters& out);

}} // namespace ss::win
