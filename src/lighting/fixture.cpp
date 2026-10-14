#include "lighting/fixture.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <set>

namespace ss {
using json = nlohmann::json;

int ChannelDef::rangeIndex(uint32_t v) const {
  if (ranges.empty()) return 0;
  for (size_t i = 0; i < ranges.size(); ++i) if (v >= ranges[i].lo && v <= ranges[i].hi) return (int)i;
  return -1;
}

int FixtureProfile::findChannel(const std::string& n) const {
  for (size_t i = 0; i < channels.size(); ++i) if (channels[i].name == n) return (int)i;
  return -1;
}

Status FixtureProfile::validate() const {
  auto bad = [&](const std::string& m) { return Status::Fail(Err::InvalidArgument, "profile '" + id + "': " + m); };
  if (id.empty()) return Status::Fail(Err::InvalidArgument, "profile id required");
  if (channels.empty()) return bad("no channels");
  if (channelCount < 1 || channelCount > 512) return bad("channelCount must be 1..512");
  std::vector<int> used((size_t)channelCount, -1);
  std::set<std::string> names;
  for (size_t i = 0; i < channels.size(); ++i) {
    const ChannelDef& c = channels[i];
    if (c.name.empty() || !names.insert(c.name).second) return bad("channel names must be unique and non-empty");
    if (c.width != 1 && c.width != 2) return bad("channel '" + c.name + "' width must be 1 or 2");
    if (c.offset < 0 || c.offset + c.width > channelCount) return bad("channel '" + c.name + "' outside footprint");
    for (int s = c.offset; s < c.offset + c.width; ++s) {
      if (used[(size_t)s] >= 0) return bad("channel '" + c.name + "' overlaps another channel");
      used[(size_t)s] = (int)i;
    }
    if (c.legalMin > c.legalMax || c.legalMax > c.maxRaw()) return bad("channel '" + c.name + "' bad legal range");
    auto in = [&](uint32_t v) { return v >= c.legalMin && v <= c.legalMax; };
    if (!in(c.defaultValue) || !in(c.safeValue) || (c.hasBlackout && !in(c.blackoutValue)))
      return bad("channel '" + c.name + "' default/safe/blackout outside legal range");
    long prev = -1;
    for (auto& r : c.ranges) {
      if (r.lo > r.hi || r.hi > c.maxRaw() || (long)r.lo <= prev) return bad("channel '" + c.name + "' ranges must be sorted, non-overlapping, within width");
      prev = (long)r.hi;
    }
  }
  return Status::Ok();
}

void encodeChannel(const ChannelDef& c, uint32_t raw, uint8_t* s) {
  if (c.width == 1) { s[c.offset] = (uint8_t)(raw & 0xFF); return; }
  uint8_t coarse = (uint8_t)((raw >> 8) & 0xFF), fine = (uint8_t)(raw & 0xFF);
  if (c.order == ByteOrder::CoarseFirst) { s[c.offset] = coarse; s[c.offset + 1] = fine; }
  else { s[c.offset] = fine; s[c.offset + 1] = coarse; }
}

uint32_t decodeChannel(const ChannelDef& c, const uint8_t* s) {
  if (c.width == 1) return s[c.offset];
  uint32_t a = s[c.offset], b = s[c.offset + 1];
  return c.order == ByteOrder::CoarseFirst ? (a << 8) | b : (b << 8) | a;
}

uint32_t fadeValue(const ChannelDef& c, uint32_t from, uint32_t to, Ns elapsed, Ns dur) {
  if (dur <= 0 || elapsed >= dur) return to;
  if (elapsed <= 0 || from == to) return from;
  int rf = c.rangeIndex(from), rt = c.rangeIndex(to);
  if (rf < 0 || rt < 0 || rf != rt) return from;                       // different functions: hold, then step at end
  if (!c.ranges.empty() && !c.ranges[(size_t)rf].continuous) return from; // macro range: no interpolation
  int64_t diff = (int64_t)to - (int64_t)from;
  // round-to-nearest, symmetric
  int64_t num = diff * (int64_t)(elapsed / 1000), den = (int64_t)(dur / 1000);   // microsecond scale avoids int64 overflow
  if (den <= 0) return to;
  int64_t q = (num >= 0 ? (num + den / 2) : (num - den / 2)) / den;
  int64_t v = (int64_t)from + q;
  int64_t lo = std::min(from, to), hi = std::max(from, to);
  return (uint32_t)std::max(lo, std::min(hi, v));
}

// ---------------------------------------------------------------- JSON
static const char* kFnNames[] = {"intensity", "color", "strobe", "pan", "tilt", "macro", "generic"};
static const char* kColNames[] = {"none", "red", "green", "blue", "white", "amber", "uv"};
template <size_t N> static int idxOf(const char* const (&a)[N], const std::string& s) { for (size_t i = 0; i < N; ++i) if (s == a[i]) return (int)i; return -1; }

enum class R { Missing, Ok, Bad };
static R gStr(const json& o, const char* k, std::string& out) { auto it = o.find(k); if (it == o.end()) return R::Missing; if (!it->is_string()) return R::Bad; out = it->get<std::string>(); return R::Ok; }
static R gInt(const json& o, const char* k, int64_t& out) { auto it = o.find(k); if (it == o.end()) return R::Missing; if (!it->is_number_integer()) return R::Bad; out = it->get<int64_t>(); return R::Ok; }
static R gBool(const json& o, const char* k, bool& out) { auto it = o.find(k); if (it == o.end()) return R::Missing; if (!it->is_boolean()) return R::Bad; out = it->get<bool>(); return R::Ok; }
static R gNum(const json& o, const char* k, double& out) { auto it = o.find(k); if (it == o.end()) return R::Missing; if (!it->is_number()) return R::Bad; out = it->get<double>(); return R::Ok; }
static std::string dumpJson(const json& j) { return j.dump(2, ' ', false, json::error_handler_t::replace); }

std::string profileToJson(const FixtureProfile& p) {
  json j; j["schema"] = "stagesignal.fixture-profile"; j["version"] = 1;
  j["id"] = p.id; j["name"] = p.name; j["manufacturer"] = p.manufacturer; j["notes"] = p.notes;
  j["profile_version"] = p.version; j["channel_count"] = p.channelCount; j["qualified"] = p.qualified;
  if (p.panTilt.present) j["pan_tilt"] = {{"pan_min_deg", p.panTilt.panMinDeg}, {"pan_max_deg", p.panTilt.panMaxDeg}, {"tilt_min_deg", p.panTilt.tiltMinDeg}, {"tilt_max_deg", p.panTilt.tiltMaxDeg}};
  json chs = json::array();
  for (auto& c : p.channels) {
    json o; o["name"] = c.name; o["function"] = kFnNames[(int)c.function]; o["color"] = kColNames[(int)c.color];
    o["offset"] = c.offset; o["width"] = c.width; o["order"] = c.order == ByteOrder::CoarseFirst ? "coarse-first" : "fine-first";
    o["min"] = c.legalMin; o["max"] = c.legalMax; o["default"] = c.defaultValue; o["safe"] = c.safeValue;
    if (c.hasBlackout) o["blackout"] = c.blackoutValue;
    o["intensity"] = c.intensity; o["strobe"] = c.strobe;
    json rs = json::array();
    for (auto& r : c.ranges) rs.push_back({{"lo", r.lo}, {"hi", r.hi}, {"continuous", r.continuous}, {"label", r.label}});
    o["ranges"] = rs; chs.push_back(o);
  }
  j["channels"] = chs;
  return dumpJson(j);
}

Status profileFromJson(const std::string& text, FixtureProfile& out) {
  auto fail = [](const std::string& m) { return Status::Fail(Err::InvalidArgument, "fixture profile JSON: " + m); };
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");
  std::string schema; int64_t ver = 0;
  if (gStr(j, "schema", schema) != R::Ok || schema != "stagesignal.fixture-profile") return fail("missing/unknown schema");
  if (gInt(j, "version", ver) != R::Ok) return fail("missing version");
  if (ver > 1) return Status::Fail(Err::SchemaUnsupported, "fixture profile schema version unsupported");
  FixtureProfile p; int64_t n = 0;
  if (gStr(j, "id", p.id) != R::Ok) return fail("id required");
  if (gStr(j, "name", p.name) == R::Bad || gStr(j, "manufacturer", p.manufacturer) == R::Bad || gStr(j, "notes", p.notes) == R::Bad) return fail("bad string field");
  if (gInt(j, "profile_version", n) == R::Bad) return fail("bad profile_version"); else if (n) p.version = (int)n;
  if (gInt(j, "channel_count", n) != R::Ok) return fail("channel_count required");
    p.channelCount = (int)n;
  if (gBool(j, "qualified", p.qualified) == R::Bad) return fail("bad qualified");
  auto pt = j.find("pan_tilt");
  if (pt != j.end()) {
    if (!pt->is_object()) return fail("bad pan_tilt");
    p.panTilt.present = true;
    if (gNum(*pt, "pan_min_deg", p.panTilt.panMinDeg) == R::Bad || gNum(*pt, "pan_max_deg", p.panTilt.panMaxDeg) == R::Bad ||
        gNum(*pt, "tilt_min_deg", p.panTilt.tiltMinDeg) == R::Bad || gNum(*pt, "tilt_max_deg", p.panTilt.tiltMaxDeg) == R::Bad) return fail("bad pan_tilt value");
  }
  auto chs = j.find("channels");
  if (chs == j.end() || !chs->is_array()) return fail("channels array required");
  for (auto& o : *chs) {
    if (!o.is_object()) return fail("channel must be object");
    ChannelDef c; std::string s;
    if (gStr(o, "name", c.name) != R::Ok) return fail("channel name required");
    if (gStr(o, "function", s) == R::Ok) { int f = idxOf(kFnNames, s); if (f < 0) return fail("unknown function '" + s + "'"); c.function = (ChannelFunction)f; } else if (gStr(o, "function", s) == R::Bad) return fail("bad function");
    if (gStr(o, "color", s) == R::Ok) { int f = idxOf(kColNames, s); if (f < 0) return fail("unknown color '" + s + "'"); c.color = (ColorComponent)f; }
    if (gInt(o, "offset", n) != R::Ok) return fail("channel offset required");
    c.offset = (int)n;
    if (gInt(o, "width", n) == R::Ok) c.width = (int)n; else if (gInt(o, "width", n) == R::Bad) return fail("bad width");
    if (gStr(o, "order", s) == R::Ok) { if (s == "coarse-first") c.order = ByteOrder::CoarseFirst; else if (s == "fine-first") c.order = ByteOrder::FineFirst; else return fail("unknown order"); }
    c.legalMax = c.width == 2 ? 65535u : 255u;
    int64_t v;
    R r;
    if ((r = gInt(o, "min", v)) == R::Ok) c.legalMin = (uint32_t)std::max<int64_t>(0, v); else if (r == R::Bad) return fail("bad min");
    if ((r = gInt(o, "max", v)) == R::Ok) c.legalMax = (uint32_t)std::max<int64_t>(0, v); else if (r == R::Bad) return fail("bad max");
    c.defaultValue = c.legalMin;
    if ((r = gInt(o, "default", v)) == R::Ok) c.defaultValue = (uint32_t)std::max<int64_t>(0, v); else if (r == R::Bad) return fail("bad default");
    c.safeValue = c.defaultValue;
    if ((r = gInt(o, "safe", v)) == R::Ok) c.safeValue = (uint32_t)std::max<int64_t>(0, v); else if (r == R::Bad) return fail("bad safe");
    if ((r = gInt(o, "blackout", v)) == R::Ok) { c.hasBlackout = true; c.blackoutValue = (uint32_t)std::max<int64_t>(0, v); } else if (r == R::Bad) return fail("bad blackout");
    if (gBool(o, "intensity", c.intensity) == R::Bad || gBool(o, "strobe", c.strobe) == R::Bad) return fail("bad designation flag");
    auto rs = o.find("ranges");
    if (rs != o.end()) {
      if (!rs->is_array()) return fail("ranges must be array");
      for (auto& ro : *rs) {
        FunctionRange fr; int64_t lo, hi;
        if (!ro.is_object() || gInt(ro, "lo", lo) != R::Ok || gInt(ro, "hi", hi) != R::Ok || lo < 0 || hi < 0) return fail("bad range");
        fr.lo = (uint32_t)lo; fr.hi = (uint32_t)hi;
        if (gBool(ro, "continuous", fr.continuous) == R::Bad || gStr(ro, "label", fr.label) == R::Bad) return fail("bad range field");
        c.ranges.push_back(fr);
      }
    }
    p.channels.push_back(c);
  }
  Status st = p.validate(); if (!st.ok()) return st;
  out = p; return Status::Ok();
}

// ---------------------------------------------------------------- library
Status ProfileLibrary::add(const FixtureProfile& p) {
  Status s = p.validate(); if (!s.ok()) return s;
  if (m_.count(p.id)) return Status::Fail(Err::InvalidArgument, "duplicate profile id '" + p.id + "'");
  m_[p.id] = p; return Status::Ok();
}
Status ProfileLibrary::addJson(const std::string& text) { FixtureProfile p; Status s = profileFromJson(text, p); return s.ok() ? add(p) : s; }
const FixtureProfile* ProfileLibrary::find(const std::string& id) const { auto it = m_.find(id); return it == m_.end() ? nullptr : &it->second; }
std::vector<std::string> ProfileLibrary::ids() const { std::vector<std::string> v; for (auto& kv : m_) v.push_back(kv.first); return v; }

static ChannelDef mk(const char* name, ChannelFunction f, int off) { ChannelDef c; c.name = name; c.function = f; c.offset = off; return c; }
static ChannelDef dimmerCh(int off) { ChannelDef c = mk("dimmer", ChannelFunction::Intensity, off); c.intensity = true; c.hasBlackout = true; return c; }
static ChannelDef colorCh(const char* n, ColorComponent k, int off) { ChannelDef c = mk(n, ChannelFunction::Color, off); c.color = k; c.hasBlackout = true; return c; }
static ChannelDef strobeCh(int off) {
  ChannelDef c = mk("strobe", ChannelFunction::Strobe, off); c.strobe = true;   // 0 = no strobe (safe)
  FunctionRange a; a.lo = 0; a.hi = 9; a.continuous = false; a.label = "no strobe";
  FunctionRange b; b.lo = 10; b.hi = 255; b.continuous = true; b.label = "strobe slow to fast";
  c.ranges.push_back(a); c.ranges.push_back(b); return c;
}
static FixtureProfile mkProfile(const char* id, const char* name, std::vector<ChannelDef> ch) {
  FixtureProfile p; p.id = id; p.name = name; p.manufacturer = "Generic"; p.channels = std::move(ch);
  p.channelCount = (int)p.channels.size(); p.qualified = false; p.notes = "Generic beta profile; not hardware-qualified.";
  return p;
}

const ProfileLibrary& ProfileLibrary::builtin() {
  static ProfileLibrary lib = [] {
    ProfileLibrary l;
    l.add(mkProfile("generic-dimmer", "Generic dimmer (1ch)", {dimmerCh(0)}));
    l.add(mkProfile("generic-rgb", "Generic RGB (3ch)", {colorCh("red", ColorComponent::Red, 0), colorCh("green", ColorComponent::Green, 1), colorCh("blue", ColorComponent::Blue, 2)}));
    l.add(mkProfile("generic-rgbw", "Generic RGBW (4ch)", {colorCh("red", ColorComponent::Red, 0), colorCh("green", ColorComponent::Green, 1), colorCh("blue", ColorComponent::Blue, 2), colorCh("white", ColorComponent::White, 3)}));
    l.add(mkProfile("generic-rgb-strobe", "Generic dimmer+RGB+strobe (5ch)", {dimmerCh(0), colorCh("red", ColorComponent::Red, 1), colorCh("green", ColorComponent::Green, 2), colorCh("blue", ColorComponent::Blue, 3), strobeCh(4)}));
    l.add(mkProfile("generic-rgbw-strobe", "Generic dimmer+RGBW+strobe (6ch)", {dimmerCh(0), colorCh("red", ColorComponent::Red, 1), colorCh("green", ColorComponent::Green, 2), colorCh("blue", ColorComponent::Blue, 3), colorCh("white", ColorComponent::White, 4), strobeCh(5)}));
    return l;
  }();
  return lib;
}

// ---------------------------------------------------------------- patch
PatchReport Patch::validate(const ProfileLibrary& lib, int minU, int maxU) const {
  PatchReport r;
  std::set<std::string> ids;
  std::vector<const FixtureProfile*> profs(entries.size(), nullptr);
  for (size_t i = 0; i < entries.size(); ++i) {
    const PatchEntry& e = entries[i];
    std::string tag = "fixture '" + e.fixtureId + "': ";
    if (e.fixtureId.empty() || !ids.insert(e.fixtureId).second) { r.errors.push_back(tag + "fixture id must be unique and non-empty"); continue; }
    const FixtureProfile* p = lib.find(e.profileId);
    if (!p) { r.errors.push_back(tag + "unknown profile '" + e.profileId + "'"); continue; }
    profs[i] = p;
    if (e.universe < minU || e.universe > maxU) r.errors.push_back(tag + "universe " + std::to_string(e.universe) + " outside protocol range " + std::to_string(minU) + ".." + std::to_string(maxU));
    if (e.startAddress < 1 || e.startAddress > 512) r.errors.push_back(tag + "start address must be 1..512");
    else if (e.startAddress + p->footprint() - 1 > 512) r.errors.push_back(tag + "start " + std::to_string(e.startAddress) + " + footprint " + std::to_string(p->footprint()) + " exceeds 512");
  }
  for (size_t a = 0; a < entries.size(); ++a) for (size_t b = a + 1; b < entries.size(); ++b) {
    if (!profs[a] || !profs[b] || entries[a].universe != entries[b].universe) continue;
    int a0 = entries[a].startAddress, a1 = a0 + profs[a]->footprint() - 1, b0 = entries[b].startAddress, b1 = b0 + profs[b]->footprint() - 1;
    int lo = std::max(a0, b0), hi = std::min(a1, b1);
    if (lo > hi) continue;
    PatchConflict c; c.a = a; c.b = b; c.universe = entries[a].universe; c.firstSlot = lo; c.lastSlot = hi;
    Ownership oa = entries[a].ownership, ob = entries[b].ownership;
    c.resolved = (oa == Ownership::Owner && ob == Ownership::Yielded) || (oa == Ownership::Yielded && ob == Ownership::Owner);
    r.conflicts.push_back(c);
    r.warnings.push_back("patch conflict: '" + entries[a].fixtureId + "' and '" + entries[b].fixtureId + "' overlap in universe " + std::to_string(c.universe) +
                         " slots " + std::to_string(lo) + ".." + std::to_string(hi) + (c.resolved ? " (ownership resolved)" : " (UNRESOLVED: cannot arm)"));
  }
  return r;
}

static const char* kOwn[] = {"unresolved", "owner", "yielded"};
std::string Patch::toJson() const {
  json j; j["schema"] = "stagesignal.patch"; j["version"] = 1; json arr = json::array();
  for (auto& e : entries) arr.push_back({{"id", e.fixtureId}, {"profile", e.profileId}, {"label", e.label}, {"universe", e.universe}, {"address", e.startAddress}, {"ownership", kOwn[(int)e.ownership]}});
  j["fixtures"] = arr; return dumpJson(j);
}

Status Patch::fromJson(const std::string& text, Patch& out) {
  auto fail = [](const std::string& m) { return Status::Fail(Err::InvalidArgument, "patch JSON: " + m); };
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");
  std::string s; int64_t v;
  if (gStr(j, "schema", s) != R::Ok || s != "stagesignal.patch") return fail("missing/unknown schema");
  if (gInt(j, "version", v) != R::Ok) return fail("missing version");
  if (v > 1) return Status::Fail(Err::SchemaUnsupported, "patch schema version unsupported");
  auto fx = j.find("fixtures");
  if (fx == j.end() || !fx->is_array()) return fail("fixtures array required");
  Patch p;
  for (auto& o : *fx) {
    PatchEntry e;
    if (!o.is_object() || gStr(o, "id", e.fixtureId) != R::Ok || gStr(o, "profile", e.profileId) != R::Ok) return fail("fixture id/profile required");
    if (gStr(o, "label", e.label) == R::Bad) return fail("bad label");
    if (gInt(o, "universe", v) != R::Ok) return fail("universe required");
    e.universe = (int)v;
    if (gInt(o, "address", v) != R::Ok) return fail("address required");
    e.startAddress = (int)v;
    if (gStr(o, "ownership", s) == R::Ok) { int k = idxOf(kOwn, s); if (k < 0) return fail("unknown ownership"); e.ownership = (Ownership)k; }
    p.entries.push_back(e);
  }
  out = p; return Status::Ok();
}

} // namespace ss
