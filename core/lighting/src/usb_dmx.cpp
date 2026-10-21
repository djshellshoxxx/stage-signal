#include "ss/lighting/usb_dmx.hpp"

namespace ss::lighting::usb {

const std::vector<AdapterInfo>& adapterRegistry() {
    static const std::vector<AdapterInfo> reg = [] {
        std::vector<AdapterInfo> v;
        AdapterInfo pro;
        pro.id = "enttec-dmx-usb-pro";
        pro.name = "Enttec DMX USB Pro";
        pro.protocol = UsbProtocol::EnttecUsbPro;
        pro.protocolVersion = "Enttec DMX USB Pro widget API, label 6 output-only framing (API document revision not pinned)";
        pro.driver = "FTDI VCP or D2XX serial port, opened by the app layer";
        v.push_back(pro);
        AdapterInfo open;
        open.id = "enttec-open-dmx-usb";
        open.name = "Enttec Open DMX USB";
        open.protocol = UsbProtocol::OpenDmx;
        open.protocolVersion = "Open DMX raw frame: 250 kbaud 8N2 with host-generated BREAK and MAB (no widget framing)";
        open.driver = "FTDI D2XX, opened by the app layer";
        v.push_back(open);
        return v;
    }();
    return reg;
}

const AdapterInfo* findAdapter(const std::string& id) {
    for (const auto& a : adapterRegistry()) if (a.id == id) return &a;
    return nullptr;
}

bool isGenericCompatible(const std::string&) { return false; }

std::string describeAdapter(const std::string& id) {
    const AdapterInfo* a = findAdapter(id);
    if (!a) return "unsupported adapter: not in the named registry, no compatibility is claimed";
    return a->name + " | " + a->protocolVersion + " | " + a->driver + " | " + a->qualification;
}

Result<std::vector<std::uint8_t>> buildEnttecProDmx(const std::uint8_t* slots, std::size_t len) {
    if (!slots || len < 1 || len > kDmxSlots)
        return makeError(ErrorCategory::InvalidArgument, "Enttec Pro frame needs 1..512 slots");
    const std::size_t n = len < kEnttecMinSlots ? kEnttecMinSlots : len;
    const std::size_t payload = n + 1;                 // + start code
    std::vector<std::uint8_t> v;
    v.reserve(payload + 5);
    v.push_back(kEnttecStart);
    v.push_back(kEnttecLabelSendDmx);
    v.push_back(static_cast<std::uint8_t>(payload & 0xFF));         // length LSB
    v.push_back(static_cast<std::uint8_t>((payload >> 8) & 0xFF));  // length MSB
    v.push_back(0x00);                                              // DMX start code
    v.insert(v.end(), slots, slots + len);
    v.resize(5 + n, 0);                                             // zero pad up to the documented minimum
    v.push_back(kEnttecEnd);
    return v;
}

Result<OpenDmxFrame> buildOpenDmxFrame(const std::uint8_t* slots, std::size_t len) {
    if (!slots || len < 1 || len > kDmxSlots)
        return makeError(ErrorCategory::InvalidArgument, "Open DMX frame needs 1..512 slots");
    OpenDmxFrame f;
    f.bytes.reserve(len + 1);
    f.bytes.push_back(0x00);
    f.bytes.insert(f.bytes.end(), slots, slots + len);
    return f;
}

}  // namespace ss::lighting::usb
