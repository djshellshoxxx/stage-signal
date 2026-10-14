// Platform/adapter ports (spec §9: core knows no HWND/COM/FFmpeg). Implementations live in
// src/platform/windows (real) and next to each consumer (fakes/mocks used by tests).
#pragma once
#include "core/types.h"
#include <string>
#include <vector>
#include <functional>

namespace ss {

// ---- Audio capture (MOD-02, IF-01) --------------------------------------------------------
struct AudioDeviceInfo { std::string id; std::string name; bool isDefault = false; uint32_t channels = 0; uint32_t rate = 0; };
struct AudioOpenParams { std::string deviceId; uint32_t rate = 48000; /*44100|48000, 0=device mix rate*/ int channelRoute = 0; /*0=stereo,1=L->mono,2=R->mono*/ };
enum class AudioState : uint8_t { Closed, Opening, Running, Unavailable, Denied, Disconnected };
struct AudioStatus { AudioState state = AudioState::Closed; std::string deviceName, detail; uint32_t rate = 0, channels = 0; uint32_t generation = 0; uint64_t overruns = 0; };
// Contract: callback runs on the capture thread; it must only copy into bounded queues (never block/alloc heavily).
// NEVER silently selects another device (REQ-A02): on loss -> Disconnected + status callback within 1 s.
struct IAudioCapture {
  virtual ~IAudioCapture() = default;
  virtual std::vector<AudioDeviceInfo> enumerate() = 0;
  virtual Status open(const AudioOpenParams&) = 0;
  virtual void close() = 0;
  virtual AudioStatus status() const = 0;
  virtual void setBlockCallback(std::function<void(AudioBlockPtr)> cb) = 0;
  virtual void setStatusCallback(std::function<void(const AudioStatus&)> cb) = 0;
};

// ---- Displays (MOD-05) --------------------------------------------------------------------
struct DisplayInfo {
  std::string stableId;      // from device interface name/EDID hash; NEVER the monitor index (IF-06)
  std::string name;          // friendly
  int index = 0;             // enumeration order only (not persistent)
  int x = 0, y = 0, w = 0, h = 0;
  int dpi = 96; int rotation = 0; bool primary = false; bool ambiguous = false; /*identical monitors*/
};
struct IDisplayProvider {
  virtual ~IDisplayProvider() = default;
  virtual std::vector<DisplayInfo> enumerate() = 0;
  virtual void setChangeCallback(std::function<void()> cb) = 0;   // WM_DISPLAYCHANGE
};

// ---- Camera (MOD-08) ----------------------------------------------------------------------
struct CameraInfo { std::string id, name; };
struct CameraFormat { int w = 640, h = 480, fps = 15; };
enum class CameraState : uint8_t { Closed, Opening, Live, Frozen, Lost, Error };
struct CameraStatus { CameraState state = CameraState::Closed; CameraFormat negotiated; std::string detail; uint64_t frames = 0; };
struct ICamera {
  virtual ~ICamera() = default;
  virtual std::vector<CameraInfo> enumerate() = 0;
  virtual Status open(const std::string& id, CameraFormat want) = 0;   // never auto-selects an alternate
  virtual void close() = 0;
  virtual CameraStatus status() const = 0;
  virtual bool latest(Frame& out) = 0;               // newest frame (mailbox cap 2); false if none
  virtual void setStatusCallback(std::function<void(const CameraStatus&)>) = 0;
};

// ---- Video decode (MOD-07) ----------------------------------------------------------------
struct IVideoDecoder {
  virtual ~IVideoDecoder() = default;
  virtual Status open(const std::string& path, int maxW, int maxH) = 0;
  virtual bool nextFrame(Frame& out, Ns& ptsNs) = 0;  // false at EOF/fail; non-blocking caller thread = media worker
  virtual Status seekStart() = 0;
  virtual double durationSec() const = 0;
  virtual void close() = 0;
};

// ---- Text rasterization (Unicode) ---------------------------------------------------------
struct TextStyle { int px = 48; uint32_t color = 0xFFFFFF; bool bold = false; std::string font = "Segoe UI"; };
struct ITextRasterizer {
  virtual ~ITextRasterizer() = default;
  // Returns ARGB-in-0xAARRGGBB image (premultiplied NOT required; alpha in top byte) wrapped to maxW pixels.
  virtual bool rasterize(const std::u32string& text, const TextStyle&, int maxW, int& outW, int& outH, std::vector<uint32_t>& argb) = 0;
};

// ---- MIDI port (MOD-09) -------------------------------------------------------------------
struct MidiPortInfo { std::string stableId; std::string name; bool input = true; };
struct IMidiPort {
  virtual ~IMidiPort() = default;
  virtual std::vector<MidiPortInfo> enumerate() = 0;
  virtual Status openInput(const std::string& id, std::function<void(Ns arrivalNs, const uint8_t* d, size_t n)> cb) = 0;
  virtual Status openOutput(const std::string& id) = 0;
  virtual Status send(const uint8_t* d, size_t n) = 0;       // worker thread only (SysEx may block)
  virtual void closeInput() = 0; virtual void closeOutput() = 0;
  virtual bool inputAlive() const = 0; virtual bool outputAlive() const = 0;
  virtual void setLostCallback(std::function<void(bool input)>) = 0;
};

// ---- Lighting datagram sink (MOD-10) ------------------------------------------------------
// The ONLY object that may put lighting bytes on a socket. Preview/benchmark use CountingSink/mock.
struct IDatagramSink {
  virtual ~IDatagramSink() = default;
  virtual Status open(const std::string& nicAddr /*"" = default*/, bool allowBroadcast) = 0;
  virtual Status sendTo(const std::string& host, uint16_t port, const uint8_t* d, size_t n) = 0;
  virtual void close() = 0;
  virtual uint64_t datagramsSent() const = 0;
};

// ---- Secrets (MOD-15) ---------------------------------------------------------------------
struct ISecretStore {
  virtual ~ISecretStore() = default;
  virtual Status put(const std::string& ref, const std::string& secret) = 0;   // machine/user-local (DPAPI)
  virtual Status get(const std::string& ref, std::string& out) = 0;
  virtual Status remove(const std::string& ref) = 0;
};

} // namespace ss
