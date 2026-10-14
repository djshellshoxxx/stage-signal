#include "lighting/usb_dmx.h"
#include <algorithm>
#include <cctype>

namespace ss {

namespace enttec {
bool buildSendDmxPacket(const uint8_t* slots, size_t n, std::vector<uint8_t>& out) {
  if (n == 0 || n > kMaxSlots || !slots) return false;
  size_t count = std::max(n, kMinSlots);
  size_t len = count + 1;                       // + start code
  out.assign(len + 5, 0);
  out[0] = kStartOfMessage; out[1] = kLabelSendDmx;
  out[2] = (uint8_t)(len & 0xFF); out[3] = (uint8_t)(len >> 8);
  out[4] = 0x00;                                // DMX start code
  for (size_t i = 0; i < n; ++i) out[5 + i] = slots[i];
  out[4 + len] = kEndOfMessage;
  return true;
}
}

UsbAdapterInfo EnttecProDevice::info() const { return UsbDmxAdapterRegistry::instance().namedUnqualified().front(); }

Status EnttecProDevice::sendDmx(const uint8_t* slots, size_t n) {
  if (!link_.isOpen()) return Status::Fail(Err::DeviceLost, "USB DMX port not open");
  if (!enttec::buildSendDmxPacket(slots, n, buf_)) return Status::Fail(Err::InvalidArgument, "bad DMX length");
  return link_.write(buf_.data(), buf_.size());
}

const UsbDmxAdapterRegistry& UsbDmxAdapterRegistry::instance() { static UsbDmxAdapterRegistry r; return r; }

std::vector<UsbAdapterInfo> UsbDmxAdapterRegistry::qualified() const { return {}; }

std::vector<UsbAdapterInfo> UsbDmxAdapterRegistry::namedUnqualified() const {
  UsbAdapterInfo e;
  e.id = "enttec-dmx-usb-pro";
  e.displayName = "ENTTEC DMX USB Pro (open widget API) - not hardware-qualified";
  e.vendor = "ENTTEC";
  e.protocol = "ENTTEC DMX USB Pro widget API, Label 6 Send DMX (0x7E ... 0xE7)";
  e.protocolVersion = "open widget API (label 6 output only)";
  e.driver = "FTDI virtual COM port (VCP); serial transport supplied by the platform layer";
  e.hardwareQualified = false;
  e.qualificationStatus = "not hardware-qualified: packet framing golden-tested only; no real-hardware run recorded";
  return std::vector<UsbAdapterInfo>(1, e);
}

std::string UsbDmxAdapterRegistry::explainUnsupported(const std::string& nameOrId) const {
  std::string low = nameOrId; for (auto& c : low) c = (char)std::tolower((unsigned char)c);
  if (low.find("enttec") != std::string::npos && low.find("pro") != std::string::npos)
    return "ENTTEC DMX USB Pro: protocol framing is implemented but NOT hardware-qualified; it is not offered as a supported adapter until real-hardware results are recorded.";
  return "'" + nameOrId + "' is not a named, hardware-qualified USB DMX adapter. A USB serial port alone is not evidence of compatibility; "
         "no generic USB DMX compatibility is claimed. Use Art-Net or sACN, or add a named protocol implementation with hardware qualification.";
}

} // namespace ss
