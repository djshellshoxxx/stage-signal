// Golden packet tests for Art-Net, sACN, ENTTEC framing + USB registry (REQ-L02, REQ-L06).
#include "test.h"
#include "lighting/artnet.h"
#include "lighting/sacn.h"
#include "lighting/usb_dmx.h"
#include "lighting/mock_sink.h"
#include <cstring>
using namespace ss;

static bool eq(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) { return a == b; }

TEST(artnet_dmx_golden) {
  // port-address 0x1234 -> Net 0x12, SubUni 0x34 ; seq 7 ; physical 0 ; 4 slots
  const uint8_t gold[] = {0x41,0x72,0x74,0x2D,0x4E,0x65,0x74,0x00,  0x00,0x50,  0x00,0x0E,  0x07,  0x00,  0x34,  0x12,  0x00,0x04,  1,2,3,4};
  uint8_t d[4] = {1,2,3,4}, out[600];
  artnet::DmxParams p; p.portAddress = 0x1234; p.sequence = 7;
  size_t n = artnet::buildArtDmx(p, d, 4, out, sizeof out);
  CHECK(n == sizeof gold);
  CHECK(std::memcmp(out, gold, sizeof gold) == 0);
  artnet::ArtDmxPacket pk; CHECK(artnet::parseArtDmx(gold, sizeof gold, pk));
  CHECK(pk.p.portAddress == 0x1234 && pk.p.sequence == 7 && pk.length == 4 && pk.data[3] == 4);
}
TEST(artnet_dmx_512_and_bounds) {
  std::vector<uint8_t> d(512, 0xAA), out(700);
  artnet::DmxParams p; p.portAddress = 0x7FFF; p.sequence = 255; p.physical = 3;
  size_t n = artnet::buildArtDmx(p, d.data(), 512, out.data(), out.size());
  CHECK(n == 530);
  CHECK(out[14] == 0xFF && out[15] == 0x7F && out[16] == 0x02 && out[17] == 0x00 && out[13] == 3);   // big-endian length 0x0200
  CHECK(artnet::buildArtDmx(p, d.data(), 3, out.data(), out.size()) == 0);      // odd
  CHECK(artnet::buildArtDmx(p, d.data(), 0, out.data(), out.size()) == 0);
  CHECK(artnet::buildArtDmx(p, d.data(), 514, out.data(), out.size()) == 0);
  p.portAddress = 0x8000; CHECK(artnet::buildArtDmx(p, d.data(), 2, out.data(), out.size()) == 0);   // 16-bit address invalid
  p.portAddress = 0; CHECK(artnet::buildArtDmx(p, d.data(), 2, out.data(), 19) == 0);                // cap too small
  CHECK(artnet::buildArtDmx(p, d.data(), 2, out.data(), 20) == 20);
  CHECK(artnet::nextSequence(255) == 1 && artnet::nextSequence(1) == 2);
}
TEST(artnet_universe_display_is_wire_address) {
  CHECK(artnet::formatUniverse(0) == "0:0:0 (port-address 0)");
  CHECK(artnet::formatUniverse(0x1234) == "18:3:4 (port-address 4660)");
  CHECK(artnet::formatUniverse(18) == "0:1:2 (port-address 18)");
}
TEST(artnet_poll_golden_and_reply) {
  const uint8_t gold[] = {0x41,0x72,0x74,0x2D,0x4E,0x65,0x74,0x00, 0x00,0x20, 0x00,0x0E, 0x00, 0x00};
  uint8_t out[32]; CHECK(artnet::buildArtPoll(0, 0, out, sizeof out) == 14); CHECK(std::memcmp(out, gold, 14) == 0);
  uint8_t f, dp; CHECK(artnet::parseArtPoll(gold, 14, f, dp));
  // hand-built 239-byte reply
  std::vector<uint8_t> r(239, 0);
  std::memcpy(r.data(), "Art-Net\0", 8); r[8] = 0x00; r[9] = 0x21;
  r[10] = 192; r[11] = 168; r[12] = 1; r[13] = 50; r[14] = 0x36; r[15] = 0x19; r[16] = 0x01; r[17] = 0x02;
  r[18] = 0x05; r[19] = 0x03; r[20] = 0x12; r[21] = 0x34; r[24] = 0x78; r[25] = 0x56;
  std::memcpy(&r[26], "Node1", 5); std::memcpy(&r[44], "Long node name", 14); std::memcpy(&r[108], "#0001 ok", 8);
  r[172] = 0; r[173] = 2; r[174] = 0x80; r[190] = 0x11; r[200] = 0; r[201] = 0xDE; r[206] = 0xAD;
  artnet::PollReply pr; CHECK(artnet::parsePollReply(r.data(), r.size(), pr));
  CHECK(pr.ipString() == "192.168.1.50" && pr.port == 6454 && pr.firmware == 0x0102 && pr.netSwitch == 5 && pr.subSwitch == 3);
  CHECK(pr.oem == 0x1234 && pr.estaMan == 0x5678 && pr.shortName == "Node1" && pr.longName == "Long node name" && pr.nodeReport == "#0001 ok");
  CHECK(pr.numPorts == 2 && pr.portTypes[0] == 0x80 && pr.swOut[0] == 0x11 && pr.mac[0] == 0xDE && pr.mac[5] == 0xAD);
  CHECK(!artnet::parsePollReply(r.data(), 100, pr));
  r[9] = 0x50; CHECK(!artnet::parsePollReply(r.data(), r.size(), pr));
}
TEST(artnet_broadcast_discovery_is_opt_in) {
  RecordingSink s; s.open("", false);
  CHECK(artnet::isBroadcastAddress("255.255.255.255") && artnet::isBroadcastAddress("192.168.1.255") && !artnet::isBroadcastAddress("192.168.1.20"));
  CHECK(!artnet::sendArtPoll(s, "255.255.255.255", false).ok());
  CHECK(s.datagramsSent() == 0);
  CHECK(artnet::sendArtPoll(s, "192.168.1.20", false).ok());        // unicast default
  CHECK(artnet::sendArtPoll(s, "255.255.255.255", true).ok());
  auto r = s.records(); CHECK(r.size() == 2 && r[0].port == 6454 && r[0].bytes.size() == 14);
}

TEST(sacn_data_golden_4slots) {
  std::vector<uint8_t> gold = {
    0x00,0x10, 0x00,0x00,                                        // preamble, postamble
    'A','S','C','-','E','1','.','1','7',0,0,0,                   // ACN id
    0x70,0x72, 0x00,0x00,0x00,0x04,                              // root flags/len (114), vector
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,   // CID
    0x70,0x5C, 0x00,0x00,0x00,0x02,                              // framing flags/len (92), vector
    'S','S'};
  gold.resize(44 + 64, 0);                                       // source name padded to 64
  const uint8_t tail[] = {0x64, 0x00,0x00, 0x05, 0x00, 0x12,0x34,   // priority 100, sync 0, seq 5, options 0, universe 0x1234
                          0x70,0x0F, 0x02, 0xA1, 0x00,0x00, 0x00,0x01, 0x00,0x05, 0x00,   // DMP: flags/len 15, vec, types, addr, inc, count 5, start code
                          0x0A,0x0B,0x0C,0x0D};
  gold.insert(gold.end(), tail, tail + sizeof tail);
  CHECK(gold.size() == 130);
  sacn::SourceInfo si; si.name = "SS"; for (int i = 0; i < 16; ++i) si.cid[i] = (uint8_t)(i * 0x11);
  sacn::DataParams p; p.universe = 0x1234; p.sequence = 5;
  uint8_t d[4] = {0x0A,0x0B,0x0C,0x0D}, out[700];
  size_t n = sacn::buildDataPacket(si, p, d, 4, out, sizeof out);
  CHECK(n == 130);
  CHECK(eq(std::vector<uint8_t>(out, out + n), gold));
  sacn::ParsedData pd; CHECK(sacn::parseDataPacket(gold.data(), gold.size(), pd));
  CHECK(pd.p.universe == 0x1234 && pd.p.sequence == 5 && pd.priority == 100 && pd.name == "SS" && pd.slotCount == 4 && pd.slots[3] == 0x0D && pd.startCode == 0);
}
TEST(sacn_full_512_lengths_and_flags) {
  std::vector<uint8_t> d(512, 0x55), out(700); sacn::SourceInfo si; sacn::DataParams p; p.universe = 1;
  p.options = sacn::kOptTerminated | sacn::kOptPreview;
  size_t n = sacn::buildDataPacket(si, p, d.data(), 512, out.data(), out.size());
  CHECK(n == 638);
  CHECK(out[16] == 0x72 && out[17] == 0x6E);       // root 0x7000|622
  CHECK(out[38] == 0x72 && out[39] == 0x58);       // framing 0x7000|600
  CHECK(out[115] == 0x72 && out[116] == 0x0B);     // dmp 0x7000|523
  CHECK(out[123] == 0x02 && out[124] == 0x01);     // count 513
  CHECK(out[112] == 0xC0 && out[108] == 100);      // options bit6 terminated + bit7 preview, default priority
}
TEST(sacn_universe_bounds_and_multicast) {
  std::vector<uint8_t> d(4, 0), out(700); sacn::SourceInfo si; sacn::DataParams p;
  p.universe = 0; CHECK(sacn::buildDataPacket(si, p, d.data(), 4, out.data(), out.size()) == 0);
  p.universe = 1; CHECK(sacn::buildDataPacket(si, p, d.data(), 4, out.data(), out.size()) == 130);
  p.universe = 63999; CHECK(sacn::buildDataPacket(si, p, d.data(), 4, out.data(), out.size()) == 130);
  p.universe = 64000; CHECK(sacn::buildDataPacket(si, p, d.data(), 4, out.data(), out.size()) == 0);
  p.universe = 1; CHECK(sacn::buildDataPacket(si, p, d.data(), 0, out.data(), out.size()) == 0);
  CHECK(sacn::buildDataPacket(si, p, d.data(), 4, out.data(), 129) == 0);
  si.priority = 201; CHECK(sacn::buildDataPacket(si, p, d.data(), 4, out.data(), out.size()) == 0);
  CHECK(sacn::multicastAddress(1) == "239.255.0.1");
  CHECK(sacn::multicastAddress(63999) == "239.255.249.255");
  CHECK(sacn::multicastAddress(0x1234) == "239.255.18.52");
  CHECK(sacn::multicastAddress(0).empty() && sacn::multicastAddress(64000).empty());
}
TEST(sacn_name_truncation_and_cid) {
  sacn::SourceInfo si; si.name = std::string(80, 'x'); sacn::DataParams p; uint8_t d[2] = {0, 0}, out[200];
  size_t n = sacn::buildDataPacket(si, p, d, 2, out, sizeof out);
  CHECK(n > 0 && out[44 + 62] == 'x' && out[44 + 63] == 0);
  sacn::Cid a = sacn::makeCid(1), b = sacn::makeCid(1), c = sacn::makeCid(2);
  CHECK(a == b && a != c && (a[6] >> 4) == 4 && (a[8] & 0xC0) == 0x80);
}

TEST(enttec_pro_golden) {
  // 24 slots minimum: 4 given, zero padded. len = 25 = 0x0019
  uint8_t d[4] = {1,2,3,4}; std::vector<uint8_t> out;
  CHECK(enttec::buildSendDmxPacket(d, 4, out));
  std::vector<uint8_t> gold = {0x7E, 0x06, 0x19, 0x00, 0x00, 1,2,3,4};
  gold.resize(5 + 24, 0); gold.push_back(0xE7);
  CHECK(eq(out, gold));
  std::vector<uint8_t> big(512, 0x11);
  CHECK(enttec::buildSendDmxPacket(big.data(), 512, out));
  CHECK(out.size() == 518 && out[0] == 0x7E && out[1] == 6 && out[2] == 0x01 && out[3] == 0x02 && out[4] == 0 && out[517] == 0xE7);  // len 513 LSB first
  CHECK(!enttec::buildSendDmxPacket(big.data(), 513, out) && !enttec::buildSendDmxPacket(big.data(), 0, out));
}
struct FakeLink : IUsbSerialLink {
  bool o = false; std::vector<uint8_t> w;
  Status open(const std::string&) override { o = true; return Status::Ok(); }
  Status write(const uint8_t* d, size_t n) override { w.assign(d, d + n); return Status::Ok(); }
  void close() override { o = false; } bool isOpen() const override { return o; }
};
TEST(usb_registry_never_claims_generic) {
  auto& r = UsbDmxAdapterRegistry::instance();
  CHECK(r.qualified().empty());
  auto u = r.namedUnqualified(); CHECK(u.size() == 1);
  CHECK(!u[0].hardwareQualified && u[0].displayName.find("not hardware-qualified") != std::string::npos);
  CHECK(r.explainUnsupported("Cheap FTDI clone").find("no generic USB DMX compatibility") != std::string::npos);
  CHECK(r.explainUnsupported("ENTTEC DMX USB Pro").find("NOT hardware-qualified") != std::string::npos);
  FakeLink l; EnttecProDevice dev(l);
  uint8_t d[30] = {9};
  CHECK(!dev.sendDmx(d, 30).ok());                 // not open
  CHECK(dev.open("COM3").ok() && dev.sendDmx(d, 30).ok());
  CHECK(l.w.size() == 36 && l.w[0] == 0x7E && l.w[5] == 9 && l.w[35] == 0xE7);
  CHECK(!dev.info().hardwareQualified);
}
int main() { return ss_test::runAll(); }
