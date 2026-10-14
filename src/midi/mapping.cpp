#include "midi/mapping.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ss {

// ---- names ---------------------------------------------------------------------------------
static const char* kActionNames[] = {
  "None", "NextScene", "PrevScene", "SelectScene", "PauseAutomation", "ResumeAutomation", "ModeAuto",
  "ModeProgrammed", "ModeHybrid", "VisualBlackout", "LightingBlackout", "EmergencyBlackout",
  "RestoreBlackout", "Brightness", "Speed", "Intensity", "ColorHue", "TapTempo", "SetBpm", "ReleaseBpm",
  "SetKey", "ReleaseKey", "NextCue", "PrevCue", "TriggerCue", "PublishText", "ClearText", "SelectBank",
  "ArmLighting", "DisarmLighting", "StartRecording", "StopRecording", "AddMarker", "StartBroadcast",
  "StopBroadcast", "DisableStrobing", "SetTarget", "ResumeContinue", "ResumeNext"};
static_assert(sizeof(kActionNames) / sizeof(*kActionNames) == (size_t)ActionId::Count_, "action name table out of sync");

const char* midiTypeName(MidiMsgType t) { return t == MidiMsgType::Note ? "note" : t == MidiMsgType::CC ? "cc" : "pc"; }
const char* midiModeName(MidiMode m) {
  switch (m) {
    case MidiMode::Absolute: return "absolute";
    case MidiMode::RelativeTwosComp: return "relative_twos_complement";
    case MidiMode::RelativeBinaryOffset: return "relative_binary_offset";
    case MidiMode::RelativeSignMagnitude: return "relative_sign_magnitude";
  }
  return "absolute";
}
bool midiTypeFromName(const std::string& s, MidiMsgType& o) {
  if (s == "note") o = MidiMsgType::Note; else if (s == "cc") o = MidiMsgType::CC; else if (s == "pc") o = MidiMsgType::ProgramChange; else return false;
  return true;
}
bool midiModeFromName(const std::string& s, MidiMode& o) {
  for (int i = 0; i < 4; ++i) if (s == midiModeName((MidiMode)i)) { o = (MidiMode)i; return true; }
  return false;
}
const char* midiActionName(ActionId id) { return (size_t)id < (size_t)ActionId::Count_ ? kActionNames[(size_t)id] : "None"; }
bool midiActionFromName(const std::string& s, ActionId& o) {
  for (size_t i = 0; i < (size_t)ActionId::Count_; ++i) if (s == kActionNames[i]) { o = (ActionId)i; return true; }
  return false;
}
bool midiActionIsContinuous(ActionId id) {
  return id == ActionId::Brightness || id == ActionId::Speed || id == ActionId::Intensity || id == ActionId::ColorHue || id == ActionId::SetBpm;
}
bool midiActionIsCritical(ActionId id) {
  switch (id) {
    case ActionId::VisualBlackout: case ActionId::LightingBlackout: case ActionId::EmergencyBlackout:
    case ActionId::StopRecording: case ActionId::StopBroadcast: case ActionId::DisarmLighting:
    case ActionId::DisableStrobing: case ActionId::PauseAutomation: return true;
    default: return false;
  }
}
bool midiActionIsSceneTrigger(ActionId id) {
  switch (id) {
    case ActionId::NextScene: case ActionId::PrevScene: case ActionId::SelectScene:
    case ActionId::NextCue: case ActionId::PrevCue: case ActionId::TriggerCue: return true;
    default: return false;
  }
}
bool midiActionCarriesValue(ActionId id) {
  switch (id) {
    case ActionId::DisableStrobing: case ActionId::SelectBank: case ActionId::SetTarget: case ActionId::SetKey:
    case ActionId::SelectScene: case ActionId::TriggerCue: case ActionId::PublishText: return true;
    default: return midiActionIsContinuous(id);
  }
}

// ---- parsing -------------------------------------------------------------------------------
MidiParse parseMidiMessage(const uint8_t* d, size_t n, size_t& consumed, MidiMsg& out) {
  consumed = n ? 1 : 0;
  if (!n) return MidiParse::Invalid;
  uint8_t s = d[0];
  if (s < 0x80) { consumed = n; return MidiParse::Invalid; }            // data byte without status
  if (s >= 0xF8) return MidiParse::Realtime;
  if (s == 0xF0) { size_t i = 1; while (i < n && d[i] != 0xF7) ++i; consumed = i < n ? i + 1 : n; return MidiParse::Skipped; }
  if (s >= 0xF0) { consumed = (s == 0xF1 || s == 0xF3) ? std::min<size_t>(2, n) : s == 0xF2 ? std::min<size_t>(3, n) : 1; return MidiParse::Skipped; }
  uint8_t hi = s & 0xF0, ch = s & 0x0F;
  size_t need = (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
  if (n < need) { consumed = n; return MidiParse::Invalid; }
  consumed = need;
  for (size_t i = 1; i < need; ++i) if (d[i] & 0x80) { consumed = i; return MidiParse::Invalid; }
  out = MidiMsg();
  out.channel = ch;
  switch (hi) {
    case 0x80: out.type = MidiMsgType::Note; out.number = d[1]; out.value = d[2]; out.on = false; return MidiParse::Message;
    case 0x90: out.type = MidiMsgType::Note; out.number = d[1]; out.value = d[2]; out.on = d[2] != 0; return MidiParse::Message;  // vel 0 == note-off
    case 0xB0: out.type = MidiMsgType::CC; out.number = d[1]; out.value = d[2]; return MidiParse::Message;
    case 0xC0: out.type = MidiMsgType::ProgramChange; out.number = d[1]; out.value = 0; return MidiParse::Message;
    default: return MidiParse::Skipped;                                   // poly/channel pressure, pitch bend
  }
}

// ---- table ---------------------------------------------------------------------------------
bool MappingTable::validate(const MidiMapping& m, std::string* why) {
  auto bad = [&](const char* w) { if (why) *why = w; return false; };
  if (m.action == ActionId::None || (size_t)m.action >= (size_t)ActionId::Count_) return bad("action required");
  if (m.deviceId.empty() && m.deviceName.empty()) return bad("device id or name required");
  if (m.channel < -1 || m.channel > 15) return bad("channel out of range");
  if (m.type == MidiMsgType::ProgramChange) { if (m.number < -1 || m.number > 127) return bad("program out of range"); }
  else if (m.number < 0 || m.number > 127) return bad("number out of range");
  if (m.valueMin < 0 || m.valueMax > 127 || m.valueMin > m.valueMax) return bad("value range invalid");
  if (!std::isfinite(m.scaleMin) || !std::isfinite(m.scaleMax) || !std::isfinite(m.relStep) || m.relStep <= 0) return bad("scale invalid");
  if (m.mode != MidiMode::Absolute) {
    if (m.type != MidiMsgType::CC) return bad("relative mode requires CC");
    if (!midiActionIsContinuous(m.action)) return bad("relative mode requires a continuous action");
  }
  return true;
}
std::string MappingTable::freshId() {
  for (;;) {
    std::string id = "m" + std::to_string(++idCounter_);
    bool dup = false;
    for (auto& e : v_) if (e.m.id == id) { dup = true; break; }
    if (!dup) return id;
  }
}
std::string MappingTable::add(MidiMapping m, std::string* why) {
  if (!validate(m, why)) return "";
  std::lock_guard<std::mutex> l(m_);
  if (m.id.empty()) m.id = freshId();
  else for (auto& e : v_) if (e.m.id == m.id) { if (why) *why = "duplicate id"; return ""; }
  Entry e; e.cur = (m.scaleMin + m.scaleMax) * 0.5; e.m = m;
  v_.push_back(e);
  return m.id;
}
bool MappingTable::edit(const std::string& id, MidiMapping m, std::string* why) {
  m.id = id;
  if (!validate(m, why)) return false;
  std::lock_guard<std::mutex> l(m_);
  for (auto& e : v_) if (e.m.id == id) {
    bool reset = e.m.scaleMin != m.scaleMin || e.m.scaleMax != m.scaleMax || e.m.mode != m.mode;
    e.m = m; if (reset) e.cur = (m.scaleMin + m.scaleMax) * 0.5;
    return true;
  }
  if (why) *why = "no such mapping";
  return false;
}
bool MappingTable::remove(const std::string& id) {
  std::lock_guard<std::mutex> l(m_);
  for (size_t i = 0; i < v_.size(); ++i) if (v_[i].m.id == id) { v_.erase(v_.begin() + i); return true; }
  return false;
}
bool MappingTable::get(const std::string& id, MidiMapping& out) const {
  std::lock_guard<std::mutex> l(m_);
  for (auto& e : v_) if (e.m.id == id) { out = e.m; return true; }
  return false;
}
std::vector<MidiMapping> MappingTable::list() const {
  std::lock_guard<std::mutex> l(m_);
  std::vector<MidiMapping> r; r.reserve(v_.size());
  for (auto& e : v_) r.push_back(e.m);
  return r;
}
size_t MappingTable::size() const { std::lock_guard<std::mutex> l(m_); return v_.size(); }
void MappingTable::clear() { std::lock_guard<std::mutex> l(m_); v_.clear(); }
void MappingTable::setCurrentValue(const std::string& id, double v) {
  std::lock_guard<std::mutex> l(m_);
  for (auto& e : v_) if (e.m.id == id) e.cur = v;
}
double MappingTable::currentValue(const std::string& id) const {
  std::lock_guard<std::mutex> l(m_);
  for (auto& e : v_) if (e.m.id == id) return e.cur;
  return 0;
}

static std::string devKey(const MidiMapping& m) { return m.deviceId.empty() ? "name:" + m.deviceName : m.deviceId; }
void MappingTable::setPortAvailable(const std::string& id, const std::string& name, bool a) {
  std::lock_guard<std::mutex> l(m_);
  if (!id.empty()) dev_[id].available = a;
  if (!name.empty()) dev_["name:" + name].available = a;
  if (!a && learn_.active && ((!learn_.req.deviceId.empty() && learn_.req.deviceId == id) || (learn_.req.deviceId.empty() && learn_.req.deviceName == name)))
    learn_ = Learn();   // device lost during learn: cancel, never bind to another device
}
bool MappingTable::portAvailable(const std::string& id, const std::string& name) const {
  std::lock_guard<std::mutex> l(m_);
  auto a = dev_.find(id); if (!id.empty() && a != dev_.end() && !a->second.available) return false;
  auto b = dev_.find("name:" + name); if (!name.empty() && b != dev_.end() && !b->second.available) return false;
  return true;
}
MappingStatus MappingTable::statusLocked(const Entry& e) const {
  if (e.m.nameFallback() && !e.m.deviceConfirmed) return MappingStatus::NeedsDeviceConfirm;
  if (e.m.type == MidiMsgType::CC && !e.m.modeConfirmed) return MappingStatus::NeedsModeConfirm;
  auto it = dev_.find(devKey(e.m));
  if (it != dev_.end() && !it->second.available) return MappingStatus::DeviceUnavailable;
  return MappingStatus::Active;
}
bool MappingTable::usable(const Entry& e) const { return statusLocked(e) == MappingStatus::Active; }
MappingStatus MappingTable::status(const std::string& id) const {
  std::lock_guard<std::mutex> l(m_);
  for (auto& e : v_) if (e.m.id == id) return statusLocked(e);
  return MappingStatus::DeviceUnavailable;
}
std::vector<std::string> MappingTable::unavailableMappings() const {
  std::lock_guard<std::mutex> l(m_);
  std::vector<std::string> r;
  for (auto& e : v_) if (statusLocked(e) == MappingStatus::DeviceUnavailable) r.push_back(e.m.id);
  return r;
}

// ---- learn ---------------------------------------------------------------------------------
Status MappingTable::beginLearn(const LearnRequest& r) {
  if (r.deviceId.empty() && r.deviceName.empty()) return Status::Fail(Err::InvalidArgument, "learn needs a device");
  if (r.channel < -1 || r.channel > 15) return Status::Fail(Err::InvalidArgument, "bad channel");
  if (r.action == ActionId::None || (size_t)r.action >= (size_t)ActionId::Count_) return Status::Fail(Err::InvalidArgument, "learn needs an action");
  std::lock_guard<std::mutex> l(m_);
  learn_ = Learn(); learn_.active = true; learn_.req = r;
  return Status::Ok();
}
void MappingTable::cancelLearn() { std::lock_guard<std::mutex> l(m_); learn_ = Learn(); }
bool MappingTable::learning() const { std::lock_guard<std::mutex> l(m_); return learn_.active; }
bool MappingTable::feedLearn(const std::string& devId, const std::string& devName, const MidiMsg& m) {
  std::lock_guard<std::mutex> l(m_);
  if (!learn_.active) return false;
  const LearnRequest& r = learn_.req;
  if (!(r.deviceId.empty() ? (!devName.empty() && devName == r.deviceName) : devId == r.deviceId)) return false;
  if (r.channel >= 0 && m.channel != r.channel) return false;
  if (!learn_.captured) {
    if (m.type == MidiMsgType::Note && !m.on) return true;                // releases are never learned
    MidiMapping c;
    c.deviceId = r.deviceId; c.deviceName = r.deviceName; c.channel = m.channel; c.type = m.type; c.number = m.number;
    c.action = r.action; c.target = r.target; c.scaleMin = r.scaleMin; c.scaleMax = r.scaleMax;
    c.mode = r.mode; c.modeConfirmed = r.modeExplicit || m.type != MidiMsgType::CC;
    learn_.cand = c; learn_.captured = true; learn_.raw = learn_.lo = learn_.hi = m.value;
    return true;
  }
  const MidiMapping& c = learn_.cand;
  if (m.type == c.type && m.channel == c.channel && (m.type == MidiMsgType::ProgramChange || m.number == c.number) && !(m.type == MidiMsgType::Note && !m.on)) {
    learn_.raw = m.value; learn_.lo = std::min<int>(learn_.lo, m.value); learn_.hi = std::max<int>(learn_.hi, m.value);
  }
  return true;
}
LearnInfo MappingTable::learnInfo() const {
  std::lock_guard<std::mutex> l(m_);
  LearnInfo i; i.active = learn_.active; i.captured = learn_.captured;
  if (learn_.captured) {
    i.type = learn_.cand.type; i.channel = learn_.cand.channel; i.number = learn_.cand.number;
    i.rawValue = learn_.raw; i.minSeen = learn_.lo; i.maxSeen = learn_.hi;
    i.normalized = learn_.raw / 127.0; i.rangeLo = learn_.lo / 127.0; i.rangeHi = learn_.hi / 127.0;
  }
  return i;
}
std::string MappingTable::commitLearn(bool useObserved, std::string* why) {
  MidiMapping c;
  {
    std::lock_guard<std::mutex> l(m_);
    if (!learn_.active || !learn_.captured) { if (why) *why = "nothing learned"; return ""; }
    c = learn_.cand;
    if (useObserved && learn_.lo < learn_.hi) { c.valueMin = learn_.lo; c.valueMax = learn_.hi; }
    learn_ = Learn();
  }
  return add(c, why);
}

// ---- runtime -------------------------------------------------------------------------------
bool MappingTable::devMatch(const MidiMapping& m, const std::string& devId, const std::string& devName) const {
  return m.deviceId.empty() ? (m.deviceConfirmed && !devName.empty() && m.deviceName == devName) : m.deviceId == devId;
}
bool MappingTable::msgMatch(const Entry& e, const std::string& devId, const std::string& devName, const MidiMsg& m) const {
  const MidiMapping& x = e.m;
  if (x.type != m.type) return false;
  if (x.channel >= 0 && x.channel != m.channel) return false;
  if (!(x.type == MidiMsgType::ProgramChange ? (x.number < 0 || x.number == m.number) : x.number == m.number)) return false;
  if (m.type == MidiMsgType::Note && !m.on) return false;
  if (!devMatch(x, devId, devName)) return false;
  return usable(e);
}
MsgClass MappingTable::classify(const std::string& devId, const std::string& devName, const MidiMsg& m) const {
  std::lock_guard<std::mutex> l(m_);
  MsgClass c; bool allCont = true;
  for (auto& e : v_) if (msgMatch(e, devId, devName, m)) {
    c.mapped = true;
    if (midiActionIsCritical(e.m.action)) c.critical = true;
    if (!midiActionIsContinuous(e.m.action)) allCont = false;
    if (e.m.mode != MidiMode::Absolute) c.relative = true;
  }
  c.continuous = c.mapped && allCont;
  return c;
}
size_t MappingTable::translate(const std::string& devId, const std::string& devName, const MidiMsg& m, std::vector<MidiMatch>& out) {
  std::lock_guard<std::mutex> l(m_);
  size_t n = 0;
  for (auto& e : v_) {
    if (!msgMatch(e, devId, devName, m)) continue;
    const MidiMapping& x = e.m;
    double value = 0;
    if (x.type == MidiMsgType::ProgramChange) {
      value = x.number < 0 ? (double)m.number : x.scaleMax;
    } else if (x.mode == MidiMode::Absolute) {
      int v = m.value;
      if (v < x.valueMin || v > x.valueMax) continue;
      if (x.type == MidiMsgType::CC && v == 0 && !midiActionCarriesValue(x.action) && !(x.valueMin == 0 && x.valueMax == 0)) continue;
      int span = x.valueMax - x.valueMin;
      double norm = span ? (double)(v - x.valueMin) / span : 1.0;
      value = x.scaleMin + norm * (x.scaleMax - x.scaleMin);
    } else {
      int v = m.value, ticks = 0;
      if (x.mode == MidiMode::RelativeTwosComp) ticks = v < 64 ? v : v - 128;
      else if (x.mode == MidiMode::RelativeBinaryOffset) ticks = v - 64;
      else ticks = (v & 0x40) ? -(v & 0x3F) : (v & 0x3F);
      if (!ticks) continue;
      double lo = std::min(x.scaleMin, x.scaleMax), hi = std::max(x.scaleMin, x.scaleMax);
      e.cur = std::min(hi, std::max(lo, e.cur + ticks * x.relStep * (x.scaleMax - x.scaleMin)));
      value = e.cur;
    }
    MidiMatch mm; mm.mappingId = x.id;
    mm.action.id = x.action; mm.action.source = ActionSource::Midi; mm.action.target = x.target; mm.action.value = value;
    mm.critical = midiActionIsCritical(x.action); mm.continuous = midiActionIsContinuous(x.action);
    mm.sceneTrigger = midiActionIsSceneTrigger(x.action);
    out.push_back(std::move(mm)); ++n;
  }
  return n;
}

// ---- JSON ----------------------------------------------------------------------------------
using json = nlohmann::json;
static const char* kSchema = "stagesignal.midi.mappings";

std::string MappingTable::toJson() const {
  std::lock_guard<std::mutex> l(m_);
  json j; j["schema"] = kSchema; j["version"] = 1; j["mappings"] = json::array();
  for (auto& e : v_) {
    const MidiMapping& m = e.m;
    json o;
    o["id"] = m.id;
    o["device"] = {{"id", m.deviceId}, {"name", m.deviceName}, {"confirmed", m.deviceConfirmed}};
    o["channel"] = m.channel; o["type"] = midiTypeName(m.type); o["number"] = m.number;
    o["range"] = {m.valueMin, m.valueMax};
    o["mode"] = midiModeName(m.mode); o["modeConfirmed"] = m.modeConfirmed;
    o["action"] = midiActionName(m.action); o["target"] = m.target;
    o["scale"] = {{"min", m.scaleMin}, {"max", m.scaleMax}}; o["relStep"] = m.relStep;
    j["mappings"].push_back(o);
  }
  return j.dump(2);
}

Status MappingTable::fromJson(const std::string& text) {
  std::vector<MidiMapping> out;
  try {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return Status::Fail(Err::InvalidArgument, "not valid JSON");
    if (!j.contains("schema") || j["schema"] != kSchema) return Status::Fail(Err::SchemaUnsupported, "unknown schema");
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() != 1)
      return Status::Fail(Err::SchemaUnsupported, "unsupported version");
    if (!j.contains("mappings") || !j["mappings"].is_array()) return Status::Fail(Err::InvalidArgument, "mappings array missing");
    for (auto& o : j["mappings"]) {
      auto fail = [&](const std::string& w) { return Status::Fail(Err::InvalidArgument, "mapping " + std::to_string(out.size()) + ": " + w); };
      if (!o.is_object()) return fail("not an object");
      MidiMapping m;
      m.id = o.value("id", std::string());
      if (!o.contains("device") || !o["device"].is_object()) return fail("device missing");
      m.deviceId = o["device"].value("id", std::string()); m.deviceName = o["device"].value("name", std::string());
      m.deviceConfirmed = o["device"].value("confirmed", false);
      if (!o.contains("channel") || !o["channel"].is_number_integer()) return fail("channel missing");
      m.channel = o["channel"].get<int>();
      if (!o.contains("type") || !o["type"].is_string() || !midiTypeFromName(o["type"].get<std::string>(), m.type)) return fail("bad type");
      if (!o.contains("number") || !o["number"].is_number_integer()) return fail("number missing");
      m.number = o["number"].get<int>();
      if (o.contains("range")) {
        auto& r = o["range"];
        if (!r.is_array() || r.size() != 2 || !r[0].is_number_integer() || !r[1].is_number_integer()) return fail("bad range");
        m.valueMin = r[0].get<int>(); m.valueMax = r[1].get<int>();
      }
      if (!o.contains("mode") || !o["mode"].is_string() || !midiModeFromName(o["mode"].get<std::string>(), m.mode)) return fail("mode must be explicit and valid");
      m.modeConfirmed = o.value("modeConfirmed", false);
      if (!o.contains("action") || !o["action"].is_string() || !midiActionFromName(o["action"].get<std::string>(), m.action)) return fail("unknown action");
      m.target = o.value("target", -1);
      if (o.contains("scale")) {
        auto& s = o["scale"];
        if (!s.is_object() || !s.contains("min") || !s.contains("max") || !s["min"].is_number() || !s["max"].is_number()) return fail("bad scale");
        m.scaleMin = s["min"].get<double>(); m.scaleMax = s["max"].get<double>();
      }
      m.relStep = o.value("relStep", 1.0 / 127.0);
      std::string why;
      if (!validate(m, &why)) return fail(why);
      if (m.id.empty()) return fail("id missing");
      for (auto& p : out) if (p.id == m.id) return fail("duplicate id");
      out.push_back(std::move(m));
    }
  } catch (...) {
    return Status::Fail(Err::InvalidArgument, "malformed mapping JSON");
  }
  std::lock_guard<std::mutex> l(m_);
  v_.clear();
  for (auto& m : out) {
    Entry e; e.cur = (m.scaleMin + m.scaleMax) * 0.5; e.m = m; v_.push_back(e);
    if (m.id.size() > 1 && m.id[0] == 'm') { uint64_t n = (uint64_t)std::strtoull(m.id.c_str() + 1, nullptr, 10); if (n > idCounter_) idCounter_ = n; }
  }
  return Status::Ok();
}

} // namespace ss
