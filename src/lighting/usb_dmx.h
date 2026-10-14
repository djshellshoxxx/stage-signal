// USB DMX adapters (REQ-L06). Rules: an adapter is listed as supported ONLY if it is NAMED and has recorded
// hardware qualification results. A USB serial port alone is never evidence of compatibility; nothing here
// is ever called "generic compatible".
//  - UsbDmxAdapterRegistry::qualified()  : named + hardware-qualified adapters. INITIALLY EMPTY.
//  - UsbDmxAdapterRegistry::namedUnqualified(): protocol implemented + golden-tested, NOT hardware-qualified.
//  - explainUnsupported(): honest reason text for anything not in the qualified list.
// IUsbDmxDevice is the port the engine writes through; platform code implements the serial transport (IUsbSerialLink).
#pragma once
#include "core/types.h"
#include <functional>
#include <string>
#include <vector>

namespace ss {

struct UsbAdapterInfo {
  std::string id;                 // stable registry id, e.g. "enttec-dmx-usb-pro"
  std::string displayName;        // e.g. "ENTTEC DMX USB Pro (open widget API) - not hardware-qualified"
  std::string vendor;
  std::string protocol;           // named wire protocol
  std::string protocolVersion;
  std::string driver;             // driver/transport expected
  bool hardwareQualified = false;
  std::string qualificationStatus;               // human readable
  std::vector<std::string> qualificationResults; // recorded test results (empty while unqualified)
};

struct IUsbDmxDevice {
  virtual ~IUsbDmxDevice() = default;
  virtual UsbAdapterInfo info() const = 0;
  virtual Status open(const std::string& portName) = 0;
  virtual void close() = 0;
  virtual bool isOpen() const = 0;
  virtual Status sendDmx(const uint8_t* slots, size_t n) = 0;   // one 512-slot universe, start code 0 added by the device layer
};

// Platform-implemented byte pipe (e.g. COM port at the adapter's baud). Core never touches serial APIs.
struct IUsbSerialLink {
  virtual ~IUsbSerialLink() = default;
  virtual Status open(const std::string& portName) = 0;
  virtual Status write(const uint8_t* d, size_t n) = 0;
  virtual void close() = 0;
  virtual bool isOpen() const = 0;
};

namespace enttec {
// "ENTTEC DMX USB Pro" open widget API, Label 6 (Output Only Send DMX Packet Request):
//   0x7E | label=6 | len LSB | len MSB | start code(0) + DMX data | 0xE7
// Payload = 1 start-code byte + N data bytes, N padded with zeros to >=24 and <=512 (widget API limits).
constexpr uint8_t kStartOfMessage = 0x7E, kEndOfMessage = 0xE7, kLabelSendDmx = 6;
constexpr size_t kMinSlots = 24, kMaxSlots = 512;
// Returns false if n==0 or n>512.
bool buildSendDmxPacket(const uint8_t* slots, size_t n, std::vector<uint8_t>& out);
}

// Device on top of an IUsbSerialLink. NOT hardware-qualified: framing is golden-tested only.
class EnttecProDevice : public IUsbDmxDevice {
 public:
  explicit EnttecProDevice(IUsbSerialLink& link) : link_(link) {}
  UsbAdapterInfo info() const override;
  Status open(const std::string& portName) override { return link_.open(portName); }
  void close() override { link_.close(); }
  bool isOpen() const override { return link_.isOpen(); }
  Status sendDmx(const uint8_t* slots, size_t n) override;
 private:
  IUsbSerialLink& link_; std::vector<uint8_t> buf_;
};

class UsbDmxAdapterRegistry {
 public:
  static const UsbDmxAdapterRegistry& instance();
  std::vector<UsbAdapterInfo> qualified() const;          // ONLY named + hardware-qualified (initially empty)
  std::vector<UsbAdapterInfo> namedUnqualified() const;   // named, protocol implemented, "not hardware-qualified"
  // Explanation for an adapter name/id not in qualified(). Never claims generic compatibility.
  std::string explainUnsupported(const std::string& nameOrId) const;
};

} // namespace ss
