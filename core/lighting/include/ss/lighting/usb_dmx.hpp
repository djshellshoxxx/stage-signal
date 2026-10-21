// Named USB-DMX adapter registry (REQ-L06) and serial frame builders. DATA ONLY: this module never opens a
// serial port; the app layer owns the device. A USB serial port alone is not compatibility evidence, so every
// registry entry is named, carries its protocol/version and an honest qualification string.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/lighting/limits.hpp"

namespace ss::lighting::usb {

enum class UsbProtocol : std::uint8_t { EnttecUsbPro, OpenDmx };

// Exact text used until a real hardware test has been recorded for the entry.
constexpr const char* kUnqualified = "unqualified: no hardware test";

struct AdapterInfo {
    std::string id;                  // stable key stored in show/local settings
    std::string name;                // the named product, never "generic"
    UsbProtocol protocol = UsbProtocol::EnttecUsbPro;
    std::string protocolVersion;     // framing/protocol description and revision basis
    std::string driver;              // OS driver the app layer needs (not exercised by this module)
    std::string qualification = kUnqualified;
    bool hardwareQualified = false;  // true only after a recorded real-hardware result
};

const std::vector<AdapterInfo>& adapterRegistry();
const AdapterInfo* findAdapter(const std::string& id);   // nullptr when unknown
// An adapter is NEVER called generic-compatible. Unknown names return an explicit unsupported message.
bool isGenericCompatible(const std::string& anyName);    // always false
std::string describeAdapter(const std::string& id);      // "name | protocol | driver | qualification"

// ---- Enttec DMX USB Pro, label 6 "Output Only Send DMX Packet Request" ----------------------
//   0x7E, label(6), lenLSB, lenMSB, [start code 0][slots...], 0xE7
// The documented payload range is 25..513 bytes, so the slot count is padded up to 24 with zeros.
constexpr std::uint8_t kEnttecStart = 0x7E, kEnttecEnd = 0xE7, kEnttecLabelSendDmx = 6;
constexpr std::size_t kEnttecMinSlots = 24;
Result<std::vector<std::uint8_t>> buildEnttecProDmx(const std::uint8_t* slots, std::size_t len);

// ---- Open DMX style raw frame (FTDI serial bit-bang class): description of what the app must clock out ----
struct OpenDmxFrame {
    std::uint32_t baud = 250000;     // 8 data bits, no parity, 2 stop bits
    std::uint32_t breakUs = 100;     // BREAK low time (DMX512-A minimum 92 us; margin used)
    std::uint32_t markAfterBreakUs = 12;   // MAB (DMX512-A minimum 12 us)
    std::vector<std::uint8_t> bytes;       // start code 0 followed by slots
};
Result<OpenDmxFrame> buildOpenDmxFrame(const std::uint8_t* slots, std::size_t len);

}  // namespace ss::lighting::usb
