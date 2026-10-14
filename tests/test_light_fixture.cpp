// Fixture profile / patch tests (REQ-L01).
#include "test.h"
#include "lighting/fixture.h"
using namespace ss;

const char* kMover = R"({
 "schema":"stagesignal.fixture-profile","version":1,"id":"test-mover","name":"Test mover","channel_count":8,
 "qualified":false,
 "pan_tilt":{"pan_min_deg":0,"pan_max_deg":540,"tilt_min_deg":0,"tilt_max_deg":270},
 "channels":[
  {"name":"dimmer","function":"intensity","offset":0,"width":1,"default":0,"safe":0,"blackout":0,"intensity":true},
  {"name":"strobe","function":"strobe","offset":1,"width":1,"default":0,"safe":0,"strobe":true,
   "ranges":[{"lo":0,"hi":7,"continuous":false,"label":"open"},{"lo":8,"hi":255,"continuous":true,"label":"strobe"}]},
  {"name":"pan","function":"pan","offset":2,"width":2,"order":"coarse-first","min":1000,"max":64000,"default":32768,"safe":32768},
  {"name":"tilt","function":"tilt","offset":4,"width":2,"order":"fine-first","default":20000,"safe":20000},
  {"name":"wheel","function":"macro","offset":6,"width":1,"default":0,"safe":10,
   "ranges":[{"lo":0,"hi":49,"continuous":false,"label":"white"},{"lo":50,"hi":99,"continuous":false,"label":"red"},{"lo":100,"hi":255,"continuous":true,"label":"rotate"}]}
 ]})";

TEST(builtin_profiles_valid) {
  auto& lib = ProfileLibrary::builtin();
  for (auto id : {"generic-dimmer", "generic-rgb", "generic-rgbw", "generic-rgb-strobe", "generic-rgbw-strobe"}) {
    const FixtureProfile* p = lib.find(id); CHECK(p != nullptr); if (p) { CHECK(p->validate().ok()); CHECK(!p->qualified); }
  }
  CHECK(lib.find("generic-rgb")->footprint() == 3 && lib.find("generic-rgbw-strobe")->footprint() == 6);
  const FixtureProfile* s = lib.find("generic-rgb-strobe");
  CHECK(s->channels[s->findChannel("strobe")].isStrobe() && s->channels[0].isIntensity());
}
TEST(profile_json_roundtrip_and_encode) {
  FixtureProfile p; Status s = profileFromJson(kMover, p); CHECK(s.ok()); if (!s.ok()) { std::printf("%s\n", s.detail.c_str()); return; }
  CHECK(p.channels.size() == 5 && p.panTilt.present && p.panTilt.panMaxDeg == 540);
  std::string j = profileToJson(p); FixtureProfile q; CHECK(profileFromJson(j, q).ok());
  CHECK(profileToJson(q) == j);
  CHECK(q.channels[2].legalMin == 1000 && q.channels[3].order == ByteOrder::FineFirst && q.channels[4].ranges.size() == 3);
  uint8_t slots[8] = {};
  encodeChannel(q.channels[2], 0x1234, slots); CHECK(slots[2] == 0x12 && slots[3] == 0x34);        // coarse first
  encodeChannel(q.channels[3], 0xABCD, slots); CHECK(slots[4] == 0xCD && slots[5] == 0xAB);        // fine first
  CHECK(decodeChannel(q.channels[2], slots) == 0x1234 && decodeChannel(q.channels[3], slots) == 0xABCD);
}
TEST(profile_json_rejects_bad_input) {
  FixtureProfile p;
  CHECK(!profileFromJson("not json", p).ok());
  CHECK(!profileFromJson("{\"schema\":\"x\",\"version\":1}", p).ok());
  CHECK(profileFromJson("{\"schema\":\"stagesignal.fixture-profile\",\"version\":9,\"id\":\"a\"}", p).code == Err::SchemaUnsupported);
  std::string bad = kMover; bad.replace(bad.find("\"safe\":32768"), 12, "\"safe\":500");   // safe below legal min 1000
  CHECK(!profileFromJson(bad, p).ok());
  FixtureProfile v; v.id = "o"; v.channelCount = 2; ChannelDef a; a.name = "a"; a.offset = 0; a.width = 2; ChannelDef b; b.name = "b"; b.offset = 1; v.channels = {a, b};
  CHECK(!v.validate().ok());   // overlap
}
TEST(patch_footprint_and_universe_bounds) {
  auto& lib = ProfileLibrary::builtin(); Patch p;
  PatchEntry e; e.fixtureId = "a"; e.profileId = "generic-rgbw"; e.universe = 1; e.startAddress = 509;   // 509..512 fits
  p.entries = {e}; CHECK(p.validate(lib, 1, 63999).ok());
  p.entries[0].startAddress = 510; auto r = p.validate(lib, 1, 63999); CHECK(!r.ok() && r.errors[0].find("exceeds 512") != std::string::npos);
  p.entries[0].startAddress = 0; CHECK(!p.validate(lib, 1, 63999).ok());
  p.entries[0].startAddress = 1; p.entries[0].universe = 0; CHECK(!p.validate(lib, 1, 63999).ok());      // sACN
  CHECK(p.validate(lib, 0, 32767).ok());                                                                  // Art-Net
  p.entries[0].universe = 32768; CHECK(!p.validate(lib, 0, 32767).ok());
  p.entries[0].universe = 63999; CHECK(p.validate(lib, 1, 63999).ok());
  p.entries[0].profileId = "nope"; CHECK(!p.validate(lib, 1, 63999).ok());
}
TEST(patch_conflicts_and_ownership) {
  auto& lib = ProfileLibrary::builtin(); Patch p;
  PatchEntry a; a.fixtureId = "a"; a.profileId = "generic-rgb"; a.universe = 1; a.startAddress = 1;
  PatchEntry b = a; b.fixtureId = "b"; b.startAddress = 3;           // overlaps slot 3
  PatchEntry c = a; c.fixtureId = "c"; c.startAddress = 6;           // adjacent, no conflict
  PatchEntry d = a; d.fixtureId = "d"; d.universe = 2; d.startAddress = 1;   // other universe
  p.entries = {a, b, c, d};
  auto r = p.validate(lib, 1, 63999);
  CHECK(r.ok() && r.conflicts.size() == 1 && r.unresolved() == 1 && !r.canArm() && r.warnings.size() == 1);
  CHECK(r.conflicts[0].firstSlot == 3 && r.conflicts[0].lastSlot == 3);
  p.entries[0].ownership = Ownership::Owner; r = p.validate(lib, 1, 63999); CHECK(!r.canArm());   // owner but other unresolved
  p.entries[1].ownership = Ownership::Yielded; r = p.validate(lib, 1, 63999); CHECK(r.canArm() && r.conflicts[0].resolved && r.warnings.size() == 1);
  p.entries[1].ownership = Ownership::Owner; r = p.validate(lib, 1, 63999); CHECK(!r.canArm());   // two owners
  p.entries.push_back(a); p.entries.back().fixtureId = "a"; CHECK(!p.validate(lib, 1, 63999).ok());   // duplicate id
}
TEST(patch_json_roundtrip) {
  Patch p; PatchEntry a; a.fixtureId = "par1"; a.profileId = "generic-rgbw"; a.universe = 7; a.startAddress = 33; a.label = "Front \xC3\xA9"; a.ownership = Ownership::Owner;
  p.entries = {a, a}; p.entries[1].fixtureId = "par2"; p.entries[1].ownership = Ownership::Yielded;
  std::string j = p.toJson(); Patch q; CHECK(Patch::fromJson(j, q).ok());
  CHECK(q.entries.size() == 2 && q.entries[0].universe == 7 && q.entries[0].startAddress == 33 && q.entries[0].label == a.label);
  CHECK(q.entries[1].ownership == Ownership::Yielded && q.toJson() == j);
  CHECK(!Patch::fromJson("{}", q).ok());
}
TEST(fade_physical_domain) {
  FixtureProfile p; CHECK(profileFromJson(kMover, p).ok());
  const ChannelDef& dim = ProfileLibrary::builtin().find("generic-dimmer")->channels[0];
  CHECK(fadeValue(dim, 0, 200, 0, kNsPerSec) == 0);
  CHECK(fadeValue(dim, 0, 200, kNsPerSec / 2, kNsPerSec) == 100);
  CHECK(fadeValue(dim, 200, 0, kNsPerSec / 4, kNsPerSec) == 150);
  CHECK(fadeValue(dim, 0, 200, kNsPerSec, kNsPerSec) == 200 && fadeValue(dim, 0, 200, 5 * kNsPerSec, kNsPerSec) == 200);
  CHECK(fadeValue(dim, 0, 200, 1, 0) == 200);                         // zero-length fade is immediate
  const ChannelDef& pan = p.channels[2];                              // 16-bit
  CHECK(fadeValue(pan, 2000, 4000, kNsPerSec / 2, kNsPerSec) == 3000);
  const ChannelDef& strobe = p.channels[1];                           // 0..7 macro, 8..255 continuous
  CHECK(fadeValue(strobe, 0, 100, kNsPerSec / 2, kNsPerSec) == 0);    // crossing macro -> hold, no interpolation through ranges
  CHECK(fadeValue(strobe, 0, 100, kNsPerSec, kNsPerSec) == 100);
  CHECK(fadeValue(strobe, 20, 100, kNsPerSec / 2, kNsPerSec) == 60);  // same continuous range -> interpolates
  const ChannelDef& wheel = p.channels[4];
  CHECK(fadeValue(wheel, 0, 40, kNsPerSec / 2, kNsPerSec) == 0);      // inside a non-continuous macro range
  CHECK(fadeValue(wheel, 60, 90, kNsPerSec / 2, kNsPerSec) == 60);
  CHECK(fadeValue(wheel, 120, 200, kNsPerSec / 2, kNsPerSec) == 160);
}
int main() { return ss_test::runAll(); }
