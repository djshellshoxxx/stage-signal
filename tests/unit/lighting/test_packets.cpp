// T-DMX: packet goldens (hand-constructed byte arrays), universe boundaries, numbering, USB frames, registry.
#include <cstring>
#include <type_traits>
#include <vector>
#include "doctest/doctest.h"
#include "ss/base/utf8.hpp"
#include "ss/lighting/addressing.hpp"
#include "ss/lighting/artnet.hpp"
#include "ss/lighting/sacn.hpp"
#include "ss/lighting/usb_dmx.hpp"

using namespace ss;
using namespace ss::lighting;
using Bytes = std::vector<std::uint8_t>;

namespace {
Bytes cat(std::initializer_list<Bytes> parts) { Bytes o; for (auto& p : parts) o.insert(o.end(), p.begin(), p.end()); return o; }
Bytes zeros(std::size_t n) { return Bytes(n, 0); }
Bytes sacnCid() { return {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F}; }
sacn::DataParams sacnParams() {
    sacn::DataParams p;
    for (std::size_t i = 0; i < 16; ++i) p.cid[i] = static_cast<std::uint8_t>(0x10 + i);
    p.sourceName = "SS"; p.priority = 100; p.sequence = 5; p.universe.value = 0x1234;
    return p;
}
}  // namespace

TEST_CASE("REQ-L02: Art-Net ArtDmx golden packet") {
    artnet::DmxParams p;
    p.address = artnetFromNetSubUni(1, 2, 3).value();   // Port-Address 0x0123
    p.sequence = 7; p.physical = 1;
    const std::uint8_t data[4] = {1, 2, 3, 4};
    auto pkt = artnet::buildDmx(p, data, 4);
    REQUIRE(pkt.ok());
    const Bytes golden = {
        0x41, 0x72, 0x74, 0x2D, 0x4E, 0x65, 0x74, 0x00,   // "Art-Net\0"
        0x00, 0x50,                                       // OpCode 0x5000, little-endian
        0x00, 0x0E,                                       // protocol version 14, hi then lo
        0x07,                                             // sequence
        0x01,                                             // physical
        0x23,                                             // SubUni = SubNet(2) << 4 | Universe(3)
        0x01,                                             // Net
        0x00, 0x04,                                       // length hi, lo (big-endian)
        0x01, 0x02, 0x03, 0x04};
    CHECK(pkt.value() == golden);
    auto back = artnet::parseDmx(pkt.value().data(), pkt.value().size());
    REQUIRE(back.ok());
    CHECK(back.value().params.address.wire == 0x0123);
    CHECK(back.value().params.sequence == 7);
    CHECK(back.value().data == Bytes(data, data + 4));
}

TEST_CASE("REQ-L02: Art-Net ArtDmx full-universe size and 15-bit port address on the wire") {
    Bytes d(512, 0xAB);
    artnet::DmxParams p; p.address.wire = 32767; p.sequence = 255;
    auto pkt = artnet::buildDmx(p, d.data(), 512);
    REQUIRE(pkt.ok());
    CHECK(pkt.value().size() == 18 + 512);
    CHECK(pkt.value()[14] == 0xFF);       // SubUni
    CHECK(pkt.value()[15] == 0x7F);       // Net, 7 bits
    CHECK(pkt.value()[16] == 0x02);       // 512 = 0x0200
    CHECK(pkt.value()[17] == 0x00);
}

TEST_CASE("REQ-L02: Art-Net length boundaries: even 2..512 only") {
    Bytes d(514, 0);
    artnet::DmxParams p;
    CHECK_FALSE(artnet::buildDmx(p, d.data(), 0).ok());
    CHECK_FALSE(artnet::buildDmx(p, d.data(), 1).ok());
    CHECK(artnet::buildDmx(p, d.data(), 2).ok());
    CHECK_FALSE(artnet::buildDmx(p, d.data(), 3).ok());
    CHECK(artnet::buildDmx(p, d.data(), 510).ok());
    CHECK(artnet::buildDmx(p, d.data(), 512).ok());
    CHECK_FALSE(artnet::buildDmx(p, d.data(), 513).ok());
    CHECK_FALSE(artnet::buildDmx(p, d.data(), 514).ok());
    CHECK_FALSE(artnet::buildDmx(p, nullptr, 2).ok());
    p.address.wire = 32768;
    CHECK_FALSE(artnet::buildDmx(p, d.data(), 2).ok());
}

TEST_CASE("REQ-L02: Art-Net sequence skips zero on wrap") {
    CHECK(artnet::nextSequence(0) == 1);
    CHECK(artnet::nextSequence(1) == 2);
    CHECK(artnet::nextSequence(254) == 255);
    CHECK(artnet::nextSequence(255) == 1);
}

TEST_CASE("REQ-L02: ArtDmx parser rejects malformed packets") {
    Bytes d(4, 9);
    artnet::DmxParams p;
    Bytes good = artnet::buildDmx(p, d.data(), 4).value();
    Bytes b = good; b[0] = 'X';
    CHECK_FALSE(artnet::parseDmx(b.data(), b.size()).ok());
    b = good; b[8] = 0x01;                                  // wrong opcode
    CHECK_FALSE(artnet::parseDmx(b.data(), b.size()).ok());
    b = good; b.pop_back();                                 // length mismatch
    CHECK_FALSE(artnet::parseDmx(b.data(), b.size()).ok());
    b = good; b[11] = 13;                                   // protocol version below 14
    CHECK_FALSE(artnet::parseDmx(b.data(), b.size()).ok());
    b = good; b[17] = 3;                                    // odd length
    CHECK_FALSE(artnet::parseDmx(b.data(), b.size()).ok());
    CHECK_FALSE(artnet::parseDmx(good.data(), 5).ok());
    CHECK_FALSE(artnet::parseDmx(nullptr, 0).ok());
}

TEST_CASE("REQ-L02: ArtPoll golden and ArtPollReply parsing (opt-in discovery)") {
    const Bytes golden = {0x41, 0x72, 0x74, 0x2D, 0x4E, 0x65, 0x74, 0x00, 0x00, 0x20, 0x00, 0x0E, 0x00, 0x10};
    CHECK(artnet::buildPoll(artnet::PollParams{}) == golden);

    Bytes r(239, 0);
    const Bytes id = {0x41, 0x72, 0x74, 0x2D, 0x4E, 0x65, 0x74, 0x00};
    std::memcpy(r.data(), id.data(), 8);
    r[8] = 0x00; r[9] = 0x21;                       // OpCode 0x2100 little-endian
    r[10] = 10; r[11] = 0; r[12] = 0; r[13] = 5;    // IP 10.0.0.5
    r[14] = 0x36; r[15] = 0x19;                     // port 0x1936, little-endian
    r[16] = 0; r[17] = 4;                           // firmware 0.4
    r[18] = 1; r[19] = 2;                           // NetSwitch 1, SubSwitch 2
    r[20] = 0x12; r[21] = 0x34;                     // OEM 0x1234
    std::memcpy(&r[26], "Node", 4);
    std::memcpy(&r[44], "Lab Node \x01 long", 15);  // control char gets sanitised
    r[172] = 0; r[173] = 2;                         // two ports
    r[174] = 0x80; r[175] = 0x40;                   // port 0 outputs, port 1 inputs
    r[186 + 1] = 0x05;                              // SwIn[1]
    r[190 + 0] = 0x03;                              // SwOut[0]
    for (int i = 0; i < 6; ++i) r[static_cast<std::size_t>(201 + i)] = static_cast<std::uint8_t>(0xA0 + i);
    r[207] = 10; r[208] = 0; r[209] = 0; r[210] = 5; r[211] = 1;   // BindIp, BindIndex
    auto info = artnet::parsePollReply(r.data(), r.size());
    REQUIRE(info.ok());
    const auto& n = info.value();
    CHECK(n.ip.toString() == "10.0.0.5");
    CHECK(n.port == 6454);
    CHECK(n.versionLo == 4);
    CHECK(n.oem == 0x1234);
    CHECK(n.shortName == "Node");
    CHECK(n.longName == "Lab Node ? long");
    CHECK(n.numPorts == 2);
    CHECK(n.portCanOutput(0));
    CHECK_FALSE(n.portCanOutput(1));
    CHECK(n.portCanInput(1));
    CHECK(n.outputAddress(0).wire == ((1 << 8) | (2 << 4) | 3));
    CHECK(n.inputAddress(1).wire == ((1 << 8) | (2 << 4) | 5));
    CHECK(n.mac[0] == 0xA0);
    CHECK(n.hasBind);
    CHECK(n.bindIndex == 1);
    // legacy 207-byte reply has no bind fields
    auto shortReply = artnet::parsePollReply(r.data(), 207);
    REQUIRE(shortReply.ok());
    CHECK_FALSE(shortReply.value().hasBind);
    CHECK_FALSE(artnet::parsePollReply(r.data(), 206).ok());
    r[9] = 0x20;
    CHECK_FALSE(artnet::parsePollReply(r.data(), r.size()).ok());
}

TEST_CASE("REQ-L02: sACN E1.31 data packet golden") {
    auto p = sacnParams();
    const std::uint8_t data[3] = {0xFF, 0x80, 0x00};
    auto pkt = sacn::buildData(p, data, 3);
    REQUIRE(pkt.ok());
    const Bytes golden = cat({
        {0x00, 0x10, 0x00, 0x00},                                                   // preamble, postamble sizes
        {0x41, 0x53, 0x43, 0x2D, 0x45, 0x31, 0x2E, 0x31, 0x37, 0x00, 0x00, 0x00},  // "ASC-E1.17"
        {0x70, 0x71},                                                               // root flags + length (129 - 16 = 113)
        {0x00, 0x00, 0x00, 0x04},                                                   // VECTOR_ROOT_E131_DATA
        sacnCid(),
        {0x70, 0x5B},                                                               // framing flags + length (129 - 38 = 91)
        {0x00, 0x00, 0x00, 0x02},                                                   // VECTOR_E131_DATA_PACKET
        {'S', 'S'}, zeros(62),                                                      // source name, 64 bytes
        {0x64},                                                                     // priority 100
        {0x00, 0x00},                                                               // synchronization address
        {0x05},                                                                     // sequence
        {0x00},                                                                     // options
        {0x12, 0x34},                                                               // universe 4660
        {0x70, 0x0E},                                                               // DMP flags + length (129 - 115 = 14)
        {0x02, 0xA1, 0x00, 0x00, 0x00, 0x01},                                       // vector, types, first addr, increment
        {0x00, 0x04},                                                               // property value count = 1 + 3
        {0x00},                                                                     // DMX start code
        {0xFF, 0x80, 0x00}});
    CHECK(pkt.value().size() == 129);
    CHECK(pkt.value() == golden);
    auto back = sacn::parseData(pkt.value().data(), pkt.value().size());
    REQUIRE(back.ok());
    CHECK(back.value().params.universe.value == 4660);
    CHECK(back.value().params.sourceName == "SS");
    CHECK(back.value().data == Bytes(data, data + 3));
}

TEST_CASE("REQ-L02: sACN full universe lengths 638 / 0x726E / 0x7258 / 0x720B") {
    Bytes d(512, 1);
    auto pkt = sacn::buildData(sacnParams(), d.data(), 512);
    REQUIRE(pkt.ok());
    const Bytes& b = pkt.value();
    CHECK(b.size() == 638);
    CHECK(b[16] == 0x72); CHECK(b[17] == 0x6E);
    CHECK(b[38] == 0x72); CHECK(b[39] == 0x58);
    CHECK(b[115] == 0x72); CHECK(b[116] == 0x0B);
    CHECK(b[123] == 0x02); CHECK(b[124] == 0x01);        // 513 property values
}

TEST_CASE("REQ-L02: sACN options: preview and stream terminated; termination packet") {
    auto p = sacnParams();
    const std::uint8_t d[2] = {1, 2};
    CHECK(sacn::buildData(p, d, 2).value()[112] == 0x00);
    p.preview = true;
    CHECK(sacn::buildData(p, d, 2).value()[112] == 0x80);
    p.preview = false; p.streamTerminated = true;
    auto t = sacn::buildData(p, d, 2).value();
    CHECK(t[112] == 0x40);
    auto back = sacn::parseData(t.data(), t.size());
    REQUIRE(back.ok());
    CHECK(back.value().params.streamTerminated);
    p.preview = true;
    CHECK(sacn::buildData(p, d, 2).value()[112] == 0xC0);
}

TEST_CASE("REQ-L02: sACN boundaries: universe 1..63999, slots 1..512, priority, CID, name") {
    Bytes d(513, 0);
    auto p = sacnParams();
    p.universe.value = 0;      CHECK_FALSE(sacn::buildData(p, d.data(), 1).ok());
    p.universe.value = 1;      CHECK(sacn::buildData(p, d.data(), 1).ok());
    p.universe.value = 63999;  CHECK(sacn::buildData(p, d.data(), 512).ok());
    p.universe.value = 64000;  CHECK_FALSE(sacn::buildData(p, d.data(), 1).ok());
    p.universe.value = 1;
    CHECK_FALSE(sacn::buildData(p, d.data(), 0).ok());
    CHECK_FALSE(sacn::buildData(p, d.data(), 513).ok());
    CHECK_FALSE(sacn::buildData(p, nullptr, 1).ok());
    p.priority = 200; CHECK(sacn::buildData(p, d.data(), 1).ok());
    p.priority = 201; CHECK_FALSE(sacn::buildData(p, d.data(), 1).ok());
    p.priority = 100;
    p.cid = sacn::Cid{}; CHECK_FALSE(sacn::buildData(p, d.data(), 1).ok());
    p = sacnParams(); p.sourceName = "bad\xFF"; CHECK_FALSE(sacn::buildData(p, d.data(), 1).ok());
    CHECK(sacn::DataParams{}.priority == 100);               // default priority
}

TEST_CASE("REQ-L02: sACN source name truncates to 63 bytes without splitting UTF-8") {
    std::string s(62, 'a'); s += "\xC3\xA9\xC3\xA9";        // 62 + two 2-byte code points = 66 bytes
    const std::string t = sacn::truncateSourceName(s);
    CHECK(t.size() == 62);
    CHECK(utf8::isValid(t));
    auto p = sacnParams(); p.sourceName = s;
    const std::uint8_t d[1] = {0};
    auto pkt = sacn::buildData(p, d, 1).value();
    CHECK(pkt[44 + 63] == 0);                                // always NUL terminated
}

TEST_CASE("REQ-L02: sACN parser rejects malformed packets") {
    const std::uint8_t d[2] = {1, 2};
    Bytes good = sacn::buildData(sacnParams(), d, 2).value();
    for (std::size_t idx : {std::size_t(1), std::size_t(5), std::size_t(17), std::size_t(41), std::size_t(118), std::size_t(125)}) {
        Bytes b = good; b[idx] ^= 0xFF;
        CHECK_FALSE(sacn::parseData(b.data(), b.size()).ok());
    }
    CHECK_FALSE(sacn::parseData(good.data(), good.size() - 1).ok());
    CHECK_FALSE(sacn::parseData(good.data(), 100).ok());
}

TEST_CASE("REQ-L02: sACN multicast address 239.255.hi.lo port 5568") {
    auto e = sacnMulticastEndpoint(SacnUniverse{1});
    CHECK(e.ip.toString() == "239.255.0.1"); CHECK(e.port == 5568);
    e = sacnMulticastEndpoint(SacnUniverse{4660});
    CHECK(e.ip.toString() == "239.255.18.52");
    e = sacnMulticastEndpoint(SacnUniverse{63999});
    CHECK(e.ip.toString() == "239.255.249.255");
}

TEST_CASE("REQ-L01: Art-Net and sACN universe numbering are explicit and never interchangeable") {
    // Art-Net: wire is zero-based, 15 bit
    CHECK(artnetFromWire(0).ok());
    CHECK(artnetFromWire(32767).ok());
    CHECK_FALSE(artnetFromWire(32768).ok());
    CHECK(artnetFromUserOneBased(1).value().wire == 0);
    CHECK(artnetFromUserOneBased(32768).value().wire == 32767);
    CHECK_FALSE(artnetFromUserOneBased(0).ok());
    CHECK_FALSE(artnetFromUserOneBased(32769).ok());
    CHECK(artnetToUserOneBased(ArtNetPortAddress{0}) == 1);
    CHECK(artnetFromNetSubUni(127, 15, 15).value().wire == 32767);
    CHECK_FALSE(artnetFromNetSubUni(128, 0, 0).ok());
    CHECK_FALSE(artnetFromNetSubUni(0, 16, 0).ok());
    CHECK_FALSE(artnetFromNetSubUni(0, 0, 16).ok());
    const auto a = artnetFromNetSubUni(1, 2, 3).value();
    CHECK(a.net() == 1); CHECK(a.subNet() == 2); CHECK(a.universe() == 3); CHECK(a.subUni() == 0x23);
    CHECK(artnetLabel(ArtNetPortAddress{0}).find("zero-based") != std::string::npos);
    CHECK(artnetLabel(ArtNetPortAddress{0}).find("one-based 1") != std::string::npos);
    // sACN: one-based, 1..63999, no zero
    CHECK_FALSE(sacnFromNumber(0).ok());
    CHECK(sacnFromNumber(1).ok());
    CHECK(sacnFromNumber(63999).ok());
    CHECK_FALSE(sacnFromNumber(64000).ok());
    CHECK(sacnLabel(SacnUniverse{1}).find("one-based") != std::string::npos);
    // distinct types: the same integer means different wire universes in the two protocols
    static_assert(!std::is_convertible<ArtNetPortAddress, SacnUniverse>::value, "must not convert");
    static_assert(!std::is_convertible<SacnUniverse, ArtNetPortAddress>::value, "must not convert");
}

TEST_CASE("REQ-L02: IPv4 parsing is strict") {
    CHECK(parseIpv4("192.168.1.20").ok());
    CHECK(parseIpv4("0.0.0.0").ok());
    for (const char* s : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.4 ", " 1.2.3.4", "a.b.c.d", "1..2.3", "1.2.3.", "01234.1.1.1"})
        CHECK_FALSE(parseIpv4(s).ok());
}

TEST_CASE("REQ-L06: Enttec DMX USB Pro label 6 golden frames") {
    const std::uint8_t d[3] = {1, 2, 3};
    auto f = usb::buildEnttecProDmx(d, 3);
    REQUIRE(f.ok());
    const Bytes golden = cat({{0x7E, 0x06, 0x19, 0x00, 0x00, 0x01, 0x02, 0x03}, zeros(21), {0xE7}});   // padded to 24 slots
    CHECK(f.value() == golden);
    Bytes full(512, 0x55);
    auto g = usb::buildEnttecProDmx(full.data(), 512);
    REQUIRE(g.ok());
    CHECK(g.value().size() == 4 + 513 + 1);
    CHECK(g.value()[2] == 0x01); CHECK(g.value()[3] == 0x02);     // 513 = 0x0201, LSB first
    CHECK(g.value().front() == 0x7E); CHECK(g.value().back() == 0xE7);
    CHECK_FALSE(usb::buildEnttecProDmx(full.data(), 0).ok());
    CHECK_FALSE(usb::buildEnttecProDmx(full.data(), 513).ok());
    CHECK_FALSE(usb::buildEnttecProDmx(nullptr, 4).ok());
}

TEST_CASE("REQ-L06: Open DMX frame is data only") {
    const std::uint8_t d[2] = {9, 8};
    auto f = usb::buildOpenDmxFrame(d, 2);
    REQUIRE(f.ok());
    CHECK(f.value().bytes == Bytes({0x00, 9, 8}));
    CHECK(f.value().baud == 250000);
    CHECK(f.value().breakUs >= 92);
    CHECK(f.value().markAfterBreakUs >= 12);
    CHECK_FALSE(usb::buildOpenDmxFrame(d, 0).ok());
    CHECK_FALSE(usb::buildOpenDmxFrame(d, 513).ok());
}

TEST_CASE("REQ-L06: adapter registry names every adapter and never claims generic compatibility") {
    const auto& reg = usb::adapterRegistry();
    REQUIRE(reg.size() >= 2);
    for (const auto& a : reg) {
        CHECK_FALSE(a.id.empty());
        CHECK_FALSE(a.name.empty());
        CHECK_FALSE(a.protocolVersion.empty());
        CHECK_FALSE(a.driver.empty());
        CHECK(a.qualification == "unqualified: no hardware test");
        CHECK_FALSE(a.hardwareQualified);
        CHECK(a.name.find("eneric") == std::string::npos);
        CHECK(usb::describeAdapter(a.id).find("unqualified: no hardware test") != std::string::npos);
        CHECK_FALSE(usb::isGenericCompatible(a.id));
        CHECK_FALSE(usb::isGenericCompatible(a.name));
    }
    CHECK(usb::findAdapter("enttec-dmx-usb-pro") != nullptr);
    CHECK(usb::findAdapter("some-usb-serial-dongle") == nullptr);
    CHECK(usb::describeAdapter("some-usb-serial-dongle").find("unsupported") != std::string::npos);
    CHECK_FALSE(usb::isGenericCompatible("any usb serial port"));
}
