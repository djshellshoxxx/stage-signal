#include "test.h"
#include "midi/mapping.h"
using namespace ss;

static MidiMapping mk(MidiMsgType ty, int num, ActionId a, MidiMode mode = MidiMode::Absolute, int ch = -1) {
  MidiMapping m; m.deviceId = "dev"; m.deviceName = "Dev"; m.type = ty; m.number = num; m.channel = ch; m.action = a;
  m.mode = mode; m.modeConfirmed = true; return m;
}
static MidiMsg cc(int ch, int n, int v) { MidiMsg m; m.type = MidiMsgType::CC; m.channel = ch; m.number = n; m.value = v; return m; }
static MidiMsg note(int ch, int n, int v, bool on = true) { MidiMsg m; m.type = MidiMsgType::Note; m.channel = ch; m.number = n; m.value = v; m.on = on; return m; }

TEST(midi_learn_edit_delete_roundtrip) {
  MappingTable t;
  LearnRequest rq; rq.deviceId = "dev"; rq.deviceName = "Dev"; rq.channel = -1; rq.action = ActionId::Brightness;
  CHECK(t.beginLearn(rq).ok());
  CHECK(t.learning());
  CHECK(!t.feedLearn("other", "Other", cc(0, 7, 10)));          // different device not consumed
  CHECK(t.feedLearn("dev", "Dev", note(2, 60, 0, false)));      // release is not learned
  CHECK(!t.learnInfo().captured);
  CHECK(t.feedLearn("dev", "Dev", cc(2, 7, 64)));
  LearnInfo li = t.learnInfo();
  CHECK(li.captured && li.type == MidiMsgType::CC && li.channel == 2 && li.number == 7 && li.rawValue == 64);
  CHECK_NEAR(li.normalized, 64 / 127.0, 1e-9);
  t.feedLearn("dev", "Dev", cc(2, 7, 10)); t.feedLearn("dev", "Dev", cc(2, 7, 100)); t.feedLearn("dev", "Dev", cc(2, 8, 5));
  li = t.learnInfo();
  CHECK(li.minSeen == 10 && li.maxSeen == 100 && li.rawValue == 100);
  std::string id = t.commitLearn(true);
  CHECK(!id.empty() && !t.learning() && t.size() == 1);
  MidiMapping m; CHECK(t.get(id, m));
  CHECK(m.channel == 2 && m.number == 7 && m.valueMin == 10 && m.valueMax == 100 && m.action == ActionId::Brightness);
  CHECK(t.status(id) == MappingStatus::NeedsModeConfirm);       // mode never guessed for CC
  std::vector<MidiMatch> out;
  CHECK(t.translate("dev", "Dev", cc(2, 7, 55), out) == 0);
  m.mode = MidiMode::Absolute; m.modeConfirmed = true; m.scaleMin = 0; m.scaleMax = 2;
  CHECK(t.edit(id, m));
  CHECK(t.status(id) == MappingStatus::Active);
  CHECK(t.translate("dev", "Dev", cc(2, 7, 55), out) == 1);
  CHECK_NEAR(out[0].action.value, 1.0, 1e-9);                   // (55-10)/90*2
  CHECK(out[0].action.source == ActionSource::Midi);
  out.clear();
  CHECK(t.translate("dev", "Dev", cc(2, 7, 5), out) == 0);       // outside range
  CHECK(t.translate("dev", "Dev", cc(3, 7, 55), out) == 0);      // wrong channel
  CHECK(t.remove(id) && !t.remove(id) && t.size() == 0);
  CHECK(t.translate("dev", "Dev", cc(2, 7, 55), out) == 0);
  CHECK(!t.commitLearn().size());
}

TEST(midi_learn_validation_and_name_fallback) {
  MappingTable t;
  LearnRequest rq; rq.action = ActionId::NextScene;
  CHECK(!t.beginLearn(rq).ok());                                 // no device
  rq.deviceName = "Pad"; CHECK(t.beginLearn(rq).ok());
  CHECK(t.feedLearn("anyid", "Pad", note(0, 36, 100)));
  std::string id = t.commitLearn();
  MidiMapping m; CHECK(t.get(id, m));
  CHECK(m.nameFallback() && m.modeConfirmed);                    // notes need no mode
  CHECK(t.status(id) == MappingStatus::NeedsDeviceConfirm);
  std::vector<MidiMatch> out;
  CHECK(t.translate("anyid", "Pad", note(0, 36, 100), out) == 0);
  m.deviceConfirmed = true; CHECK(t.edit(id, m));
  CHECK(t.translate("anyid", "Pad", note(0, 36, 100), out) == 1);
  CHECK(t.translate("anyid", "Other", note(0, 36, 100), out) == 0);
  MidiMapping bad = mk(MidiMsgType::Note, 60, ActionId::Brightness, MidiMode::RelativeTwosComp);
  std::string why; CHECK(t.add(bad, &why).empty() && !why.empty());   // relative needs CC
  bad = mk(MidiMsgType::CC, 1, ActionId::NextScene, MidiMode::RelativeTwosComp);
  CHECK(t.add(bad).empty());                                           // relative needs continuous action
}

TEST(midi_relative_encoders) {
  struct C { MidiMode mode; int up, down; } cs[] = {
    {MidiMode::RelativeTwosComp, 1, 127}, {MidiMode::RelativeBinaryOffset, 65, 63}, {MidiMode::RelativeSignMagnitude, 0x01, 0x41}};
  for (auto& c : cs) {
    MappingTable t;
    MidiMapping m = mk(MidiMsgType::CC, 10, ActionId::Speed, c.mode); m.relStep = 0.1; m.scaleMin = 0; m.scaleMax = 1;
    std::string id = t.add(m);
    std::vector<MidiMatch> o;
    CHECK(t.translate("dev", "Dev", cc(0, 10, c.up), o) == 1); CHECK_NEAR(o.back().action.value, 0.6, 1e-9);
    CHECK(t.translate("dev", "Dev", cc(0, 10, c.up), o) == 1); CHECK_NEAR(o.back().action.value, 0.7, 1e-9);
    CHECK(t.translate("dev", "Dev", cc(0, 10, c.down), o) == 1); CHECK_NEAR(o.back().action.value, 0.6, 1e-9);
    for (int i = 0; i < 20; ++i) t.translate("dev", "Dev", cc(0, 10, c.up), o);
    CHECK_NEAR(t.currentValue(id), 1.0, 1e-9);                     // clamped
    t.setCurrentValue(id, 0.25);
    o.clear(); t.translate("dev", "Dev", cc(0, 10, c.up), o); CHECK_NEAR(o[0].action.value, 0.35, 1e-9);
  }
  MappingTable t; MidiMapping m = mk(MidiMsgType::CC, 10, ActionId::Speed, MidiMode::RelativeBinaryOffset);
  t.add(m); std::vector<MidiMatch> o;
  CHECK(t.translate("dev", "Dev", cc(0, 10, 64), o) == 0);         // zero delta emits nothing
  CHECK(t.classify("dev", "Dev", cc(0, 10, 64)).relative);
}

TEST(midi_velocity_zero_is_note_off) {
  uint8_t on0[] = {0x93, 60, 0}, on[] = {0x93, 60, 90}, off[] = {0x83, 60, 40};
  size_t used; MidiMsg m;
  CHECK(parseMidiMessage(on0, 3, used, m) == MidiParse::Message && !m.on && m.channel == 3 && used == 3);
  CHECK(parseMidiMessage(on, 3, used, m) == MidiParse::Message && m.on && m.value == 90);
  CHECK(parseMidiMessage(off, 3, used, m) == MidiParse::Message && !m.on);
  uint8_t pc[] = {0xC1, 5}, clk[] = {0xF8}, sx[] = {0xF0, 1, 2, 0xF7, 0x90}, bad[] = {0x40, 1};
  CHECK(parseMidiMessage(pc, 2, used, m) == MidiParse::Message && m.type == MidiMsgType::ProgramChange && m.number == 5);
  CHECK(parseMidiMessage(clk, 1, used, m) == MidiParse::Realtime);
  CHECK(parseMidiMessage(sx, 5, used, m) == MidiParse::Skipped && used == 4);
  CHECK(parseMidiMessage(bad, 2, used, m) == MidiParse::Invalid);
  MappingTable t; t.add(mk(MidiMsgType::Note, 60, ActionId::NextScene));
  std::vector<MidiMatch> o;
  parseMidiMessage(on0, 3, used, m); CHECK(t.translate("dev", "Dev", m, o) == 0);
  CHECK(!t.classify("dev", "Dev", m).mapped);
  parseMidiMessage(on, 3, used, m); CHECK(t.translate("dev", "Dev", m, o) == 1);
  LearnRequest rq; rq.deviceId = "dev"; rq.action = ActionId::NextScene; t.beginLearn(rq);
  parseMidiMessage(on0, 3, used, m); t.feedLearn("dev", "Dev", m);
  CHECK(!t.learnInfo().captured);
}

TEST(midi_pc_mapping) {
  MappingTable t; MidiMapping m = mk(MidiMsgType::ProgramChange, -1, ActionId::SelectScene); m.scaleMax = 1; t.add(m);
  MidiMsg p; p.type = MidiMsgType::ProgramChange; p.number = 7; p.channel = 0;
  std::vector<MidiMatch> o; CHECK(t.translate("dev", "Dev", p, o) == 1 && o[0].action.value == 7.0);
}

TEST(midi_mapping_json_roundtrip) {
  MappingTable a;
  MidiMapping m = mk(MidiMsgType::CC, 21, ActionId::Brightness, MidiMode::RelativeSignMagnitude, 3);
  m.relStep = 0.02; m.scaleMin = 0.25; m.scaleMax = 3.5; m.target = 2; m.valueMin = 1; m.valueMax = 100;
  a.add(m);
  MidiMapping n = mk(MidiMsgType::Note, 36, ActionId::EmergencyBlackout); n.deviceId.clear(); n.deviceName = "My Pad"; n.deviceConfirmed = true;
  a.add(n);
  MidiMapping p = mk(MidiMsgType::ProgramChange, -1, ActionId::SelectScene); p.modeConfirmed = false; a.add(p);
  std::string js = a.toJson();
  MappingTable b;
  Status s = b.fromJson(js);
  CHECK(s.ok());
  auto la = a.list(), lb = b.list();
  CHECK(la.size() == 3 && lb.size() == 3);
  for (size_t i = 0; i < la.size() && i < lb.size(); ++i) {
    CHECK(la[i].id == lb[i].id && la[i].deviceId == lb[i].deviceId && la[i].deviceName == lb[i].deviceName);
    CHECK(la[i].deviceConfirmed == lb[i].deviceConfirmed && la[i].channel == lb[i].channel && la[i].type == lb[i].type);
    CHECK(la[i].number == lb[i].number && la[i].valueMin == lb[i].valueMin && la[i].valueMax == lb[i].valueMax);
    CHECK(la[i].mode == lb[i].mode && la[i].modeConfirmed == lb[i].modeConfirmed && la[i].action == lb[i].action);
    CHECK(la[i].target == lb[i].target && la[i].scaleMin == lb[i].scaleMin && la[i].scaleMax == lb[i].scaleMax && la[i].relStep == lb[i].relStep);
  }
  CHECK(b.toJson() == js);
  // ids stay unique after load
  CHECK(!b.add(mk(MidiMsgType::CC, 1, ActionId::Speed)).empty());
  // rejects, atomically
  size_t before = b.size();
  CHECK(b.fromJson("not json").code == Err::InvalidArgument);
  CHECK(b.fromJson("{\"schema\":\"x\",\"version\":1,\"mappings\":[]}").code == Err::SchemaUnsupported);
  CHECK(b.fromJson("{\"schema\":\"stagesignal.midi.mappings\",\"version\":2,\"mappings\":[]}").code == Err::SchemaUnsupported);
  std::string nomode = js; size_t pos = nomode.find("\"mode\""); nomode.replace(pos, 6, "\"mxde\"");
  CHECK(!b.fromJson(nomode).ok());                               // mode must be explicit
  std::string badact = js; pos = badact.find("EmergencyBlackout"); badact.replace(pos, 17, "NoSuchAction");
  CHECK(!b.fromJson(badact).ok());
  CHECK(b.size() == before);
  CHECK(b.fromJson("{\"schema\":\"stagesignal.midi.mappings\",\"version\":1,\"mappings\":[]}").ok() && b.size() == 0);
}

int main() { return ss_test::runAll(); }
