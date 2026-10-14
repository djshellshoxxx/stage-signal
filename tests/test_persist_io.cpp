#include "persistence/show_io.h"
#include "persistence/hash.h"
#include "nlohmann/json.hpp"
#include "test_show_util.h"
#include <fstream>
using namespace sst;
using namespace ss::persist;

static ShowDocument richDoc() {
  ShowDocument d = baseDoc(4);
  makeStrobe(d, 3);
  d.cues = {msCue("c0", "s0", 1500), beatCue("c1", "s1", 8), msCue("c2", "s2", 0, Quantize::Bar)};
  d.cues[0].transition = {"fade", 400}; d.cues[1].lightingRef = "look_blue"; d.cueEnd = CueListEnd::Loop;
  d.texts = {{"t1", "Welcome \xF0\x9F\x8E\xB5"}};
  d.cues[2].textRef = "t1";
  MediaRecord m; m.hash = std::string(64, 'c'); m.relPath = "media/c.png"; m.format = "png"; m.bytes = 12; d.media = {m};
  d.cues[0].mediaRef = m.hash;
  d.midiMappings = {"{\"action\":\"NextScene\",\"note\":60}"}; d.fixtureProfiles = {"{\"id\":\"dimmer\",\"channels\":1}"}; d.patch = {"{\"profile\":\"dimmer\",\"addr\":1}"};
  d.qualityProfile = "standard"; d.autoCfg.seed = 18446744073709551615ull; d.autoCfg.pool = {{"s0", 3}, {"s1", 1}};
  d.manualBpm = 128.5; d.manualKey = 14; d.disableStrobing = true; d.idle.audioLoss = AudioLossPolicy::Idle; d.idle.screen = "scene"; d.idle.sceneId = "s2";
  d.outputs[0].overlay.tempo = true; d.outputs[0].overlay.key = true; d.outputs[1].role = OutputRole::Program;
  d.scenes[0].params["bars"] = 48; d.scenes[0].params["smoothing"] = 0.6f;
  d.meta["x_vendor"] = "{\"a\":1}"; d.journal.push_back({1234, "marker", "{\"n\":1}"});
  return d;
}
static std::string write(const std::string& p, const std::string& t) { std::ofstream(p, std::ios::binary) << t; return p; }
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), {}); }

TEST(persist_roundtrip_full_document) {
  TmpDir t; ShowIO io;
  ShowDocument d = richDoc();
  CHECK(io.save(t.path("a.json"), d).ok());
  ShowDocument back; LoadInfo li;
  Status s = io.load(t.path("a.json"), back, &li);
  CHECK(s.ok());
  CHECK(encodeShow(back) == encodeShow(d));                           // lossless, including seed 2^64-1 and float params
  CHECK(back.autoCfg.seed == 18446744073709551615ull && back.manualBpm == 128.5 && back.manualKey == 14 && back.disableStrobing);
  CHECK(back.scenes[0].params.at("smoothing") == 0.6f);
  CHECK(back.meta.at("x_vendor") == "{\"a\":1}" && back.cues[1].duration.beats && back.cues[1].duration.beatCount == 8);
  CHECK(!li.decode.migrated && li.decode.warnings.empty());
  CHECK(!std::filesystem::exists(t.path("a.json.tmp")));
}

TEST(persist_checksum_tamper_and_garbage_rejected) {
  TmpDir t; ShowIO io;
  CHECK(io.save(t.path("a.json"), richDoc()).ok());
  std::string txt = slurp(t.path("a.json"));
  std::string tampered = txt; size_t p = tampered.find("Test Show"); tampered[p] = 'X';
  write(t.path("b.json"), tampered);
  ShowDocument out = baseDoc(1); out.name = "sentinel";
  CHECK(!io.load(t.path("b.json"), out).ok() && out.name == "sentinel");
  write(t.path("c.json"), "{ not json"); CHECK(!io.load(t.path("c.json"), out).ok());
  write(t.path("d.json"), "{\"format\":\"other\"}"); CHECK(!io.load(t.path("d.json"), out).ok());
  write(t.path("e.json"), std::string(200, '[') + std::string(200, ']')); CHECK(!io.load(t.path("e.json"), out).ok());   // nesting guard
  CHECK(!io.load(t.path("missing.json"), out).ok() && out.name == "sentinel");
  // valid JSON + correct checksum but semantically invalid document is rejected (full validation before swap)
  ShowDocument bad = richDoc(); bad.scenes[2].rendererType = "bogus";
  nlohmann::json env = nlohmann::json::parse(encodeShow(richDoc()));
  env["show"]["scenes"][2]["rendererType"] = "bogus";
  std::string compact = env["show"].dump(); env["checksum"] = "sha256:" + Sha256::hex(compact.data(), compact.size());
  write(t.path("f.json"), env.dump());
  Status s = io.load(t.path("f.json"), out);
  CHECK(!s.ok() && s.code == Err::InvalidArgument && out.name == "sentinel");
  CHECK(!io.save(t.path("g.json"), bad).ok() && !std::filesystem::exists(t.path("g.json")));
}

TEST(persist_invalid_import_keeps_old_show_active) {
  TmpDir t; ShowIO io;
  ShowEngine eng; CHECK(eng.replaceDocument(richDoc()).ok());
  eng.submit(act(ActionId::SelectScene, 2), 0);
  ShowSnapshot before = eng.snapshot();
  // newer major
  nlohmann::json env = nlohmann::json::parse(encodeShow(richDoc()));
  env["show"]["schema"]["major"] = 2; std::string c = env["show"].dump(); env["checksum"] = "sha256:" + Sha256::hex(c.data(), c.size());
  write(t.path("newer.json"), env.dump());
  ShowDocument incoming;
  Status s = io.load(t.path("newer.json"), incoming);
  CHECK(s.code == Err::SchemaUnsupported && !s.recoverable);
  // only swap on success (caller pattern)
  if (s.ok()) eng.replaceDocument(incoming);
  CHECK(eng.snapshot().revision == before.revision && eng.snapshot().sceneIndex == 2 && eng.document().name == "Test Show");
  // invalid-but-parseable: engine itself refuses
  ShowDocument bad = richDoc(); bad.cues[0].sceneId = "nope";
  CHECK(!eng.replaceDocument(bad).ok() && eng.snapshot().revision == before.revision);
}

TEST(persist_atomic_save_crash_restores_prior_valid) {
  TmpDir t;
  ShowIO plain;
  ShowDocument v1 = richDoc(); v1.name = "Version One";
  CHECK(plain.save(t.path("show.json"), v1).ok());
  ShowDocument v2 = richDoc(); v2.name = "Version Two";
  // crash while writing the temp file (partial bytes on disk)
  FaultFileOps fault(defaultFileOps()); fault.crashAfterBytes = 300; fault.onlyPathContaining = ".tmp";
  ShowIO io(fault);
  Status s = io.save(t.path("show.json"), v2);
  CHECK(!s.ok());
  CHECK(std::filesystem::exists(t.path("show.json.tmp")));            // the torn temp really is on disk
  ShowDocument got;
  CHECK(plain.load(t.path("show.json"), got).ok() && got.name == "Version One");   // prior valid version intact
  CHECK(!plain.load(t.path("show.json.tmp"), got).ok());              // torn file never validates
  CHECK(plain.cleanupTemps(t.path("show.json")).ok() && !std::filesystem::exists(t.path("show.json.tmp")));
  // flush failure
  FaultFileOps f2(defaultFileOps()); f2.failFlush = true; ShowIO io2(f2);
  CHECK(!io2.save(t.path("show.json"), v2).ok());
  CHECK(plain.load(t.path("show.json"), got).ok() && got.name == "Version One");
  // rename failure (first rename is the final replace when nothing needs rotating)
  FaultFileOps f3(defaultFileOps()); ShowIO io3(f3); f3.failRenameNth = 1;
  TmpDir t2; CHECK(plain.save(t2.path("s.json"), v1).ok());
  f3.failRenameNth = 3;                                               // rotation uses none for first backup; final rename is the 2nd -> pick a failing one
  for (int n = 1; n <= 3; ++n) {
    f3.renames = 0; f3.failRenameNth = n;
    Status r = io3.save(t2.path("s.json"), v2);
    if (!r.ok()) { CHECK(plain.load(t2.path("s.json"), got).ok() && got.name == "Version One"); break; }
  }
  // after a crash the next good save succeeds and wins
  CHECK(plain.save(t.path("show.json"), v2).ok());
  CHECK(plain.load(t.path("show.json"), got).ok() && got.name == "Version Two");
}

TEST(persist_retains_three_prior_valid_saves) {
  TmpDir t; ShowIO io; ShowDocument got;
  for (int i = 1; i <= 6; ++i) { ShowDocument d = richDoc(); d.name = "v" + std::to_string(i); CHECK(io.save(t.path("s.json"), d).ok()); }
  CHECK(io.load(t.path("s.json"), got).ok() && got.name == "v6");
  CHECK(io.load(ShowIO::backupPath(t.path("s.json"), 1), got).ok() && got.name == "v5");
  CHECK(io.load(ShowIO::backupPath(t.path("s.json"), 2), got).ok() && got.name == "v4");
  CHECK(io.load(ShowIO::backupPath(t.path("s.json"), 3), got).ok() && got.name == "v3");
  CHECK(!std::filesystem::exists(ShowIO::backupPath(t.path("s.json"), 4)));
  // corrupt primary -> fallback to newest valid generation
  write(t.path("s.json"), "garbage");
  LoadInfo li; CHECK(io.loadWithFallback(t.path("s.json"), got, &li).ok() && got.name == "v5" && li.recoveredFromBackup);
  // a corrupt primary is never rotated into the history by the next save
  ShowDocument d7 = richDoc(); d7.name = "v7"; CHECK(io.save(t.path("s.json"), d7).ok());
  CHECK(io.load(ShowIO::backupPath(t.path("s.json"), 1), got).ok() && got.name == "v5");
}

TEST(persist_unsupported_newer_never_falls_back_or_migrates) {
  TmpDir t; ShowIO io; ShowDocument got;
  CHECK(io.save(t.path("s.json"), richDoc()).ok());
  CHECK(io.save(t.path("s.json"), richDoc()).ok());                   // creates bak1
  nlohmann::json env = nlohmann::json::parse(slurp(t.path("s.json")));
  env["show"]["schema"]["major"] = 9; std::string c = env["show"].dump(); env["checksum"] = "sha256:" + Sha256::hex(c.data(), c.size());
  std::string newer = env.dump(); write(t.path("s.json"), newer);
  CHECK(io.loadWithFallback(t.path("s.json"), got).code == Err::SchemaUnsupported);
  CHECK(io.migrateFileInPlace(t.path("s.json")).code == Err::SchemaUnsupported);
  CHECK(slurp(t.path("s.json")) == newer);                            // source untouched
  CHECK(!std::filesystem::exists(t.path("s.json.premigrate-v9.0")));
}

TEST(persist_migration_backup_first) {
  TmpDir t; ShowIO io;
  // legacy v0 document: 'title', scenes use 'renderer'
  ShowDocument cur = baseDoc(3);
  nlohmann::json s = nlohmann::json::parse(showObjectJson(cur));
  s["schema"] = {{"major", 0}, {"minor", 0}}; s["title"] = "Legacy Show"; s.erase("name"); s.erase("migrationHistory");
  for (auto& sc : s["scenes"]) { sc["renderer"] = sc["rendererType"]; sc.erase("rendererType"); }
  std::string compact = s.dump();
  nlohmann::json env = {{"format", kShowFormat}, {"checksum", "sha256:" + Sha256::hex(compact.data(), compact.size())}, {"show", s}};
  std::string legacy = env.dump(); write(t.path("old.json"), legacy);
  ShowDocument got; LoadInfo li;
  CHECK(io.load(t.path("old.json"), got, &li).ok());                  // in-memory migration, file untouched
  CHECK(li.decode.migrated && li.decode.fromMajor == 0 && got.name == "Legacy Show" && got.scenes[1].rendererType == "plasma");
  CHECK(got.schemaMajor == kSchemaMajor && got.migrationHistory.size() == 1 && got.migrationHistory[0] == "0.0->1.0");
  CHECK(slurp(t.path("old.json")) == legacy);
  // on-disk migration: backup first, then atomic rewrite
  LoadInfo mi; CHECK(io.migrateFileInPlace(t.path("old.json"), &mi).ok());
  CHECK(mi.migratedOnDisk && slurp(mi.migrationBackup) == legacy);
  ShowDocument again; LoadInfo l2; CHECK(io.load(t.path("old.json"), again, &l2).ok() && !l2.decode.migrated && again.name == "Legacy Show");
  // backup failure aborts before anything else changes
  write(t.path("old2.json"), legacy);
  FaultFileOps f(defaultFileOps()); f.crashAfterBytes = 10; f.onlyPathContaining = "premigrate"; ShowIO bad(f);
  CHECK(!bad.migrateFileInPlace(t.path("old2.json")).ok() && slurp(t.path("old2.json")) == legacy);
}

TEST(persist_limits_enforced_before_allocation) {
  TmpDir t; ShowIO io; ShowDocument out;
  // 9 MiB file: refused by size alone (never read into memory)
  { std::ofstream f(t.path("big.json"), std::ios::binary); std::string chunk(1 << 20, 'x'); for (int i = 0; i < 9; ++i) f << chunk; }
  Status s = io.load(t.path("big.json"), out); CHECK(s.code == Err::ResourceExhausted);
  // 10001 cues in JSON: rejected from array size, before element conversion
  nlohmann::json env = nlohmann::json::parse(encodeShow(baseDoc(1)));
  nlohmann::json cues = nlohmann::json::array();
  for (int i = 0; i < 10001; ++i) cues.push_back({{"id", "c" + std::to_string(i)}});
  env["show"]["cues"] = cues; std::string c = env["show"].dump(); env["checksum"] = "sha256:" + Sha256::hex(c.data(), c.size());
  std::string txt = env.dump(); CHECK(txt.size() < 8u * 1024 * 1024);
  write(t.path("many.json"), txt);
  s = io.load(t.path("many.json"), out); CHECK(!s.ok() && s.detail.find("cues") != std::string::npos && s.detail.find("too many") != std::string::npos);
  // unknown fields: rejected at current minor
  env = nlohmann::json::parse(encodeShow(baseDoc(1)));
  env["show"]["evil"] = 1; c = env["show"].dump(); env["checksum"] = "sha256:" + Sha256::hex(c.data(), c.size());
  write(t.path("unk.json"), env.dump()); CHECK(!io.load(t.path("unk.json"), out).ok());
  // ... tolerated with a warning if written by a newer minor
  env["show"]["schema"]["minor"] = 5; c = env["show"].dump(); env["checksum"] = "sha256:" + Sha256::hex(c.data(), c.size());
  write(t.path("minor.json"), env.dump()); LoadInfo li;
  CHECK(io.load(t.path("minor.json"), out, &li).ok() && li.decode.warnings.size() == 1 && li.decode.warnings[0].find("evil") != std::string::npos);
  // x_ metadata preserved, not warned
  env = nlohmann::json::parse(encodeShow(baseDoc(1))); env["show"]["x_note"] = {{"k", 1}}; c = env["show"].dump(); env["checksum"] = "sha256:" + Sha256::hex(c.data(), c.size());
  write(t.path("meta.json"), env.dump()); CHECK(io.load(t.path("meta.json"), out, &li).ok() && out.meta.count("x_note") && li.decode.warnings.empty());
}

TEST(persist_autosave_debounce_fake_clock_no_media_copy) {
  TmpDir t; FakeClock clk; clk.t = 5 * kNsPerSec;
  FaultFileOps ops(defaultFileOps());                                 // used as a write recorder
  Autosave as(ops, t.path("auto.json"));
  ShowDocument d = richDoc();
  bool saved = true;
  CHECK(as.tick(clk.t, d, &saved).ok() && !saved);                    // not dirty: nothing
  as.markDirty(clk.t);
  clk.advance(29 * kNsPerSec); as.tick(clk.t, d, &saved); CHECK(!saved && as.saves() == 0);
  // continued edits do not postpone beyond 30 s of the first change
  as.markDirty(clk.t);
  clk.advance(1 * kNsPerSec); CHECK(as.tick(clk.t, d, &saved).ok() && saved && as.saves() == 1 && !as.dirty());
  clk.advance(60 * kNsPerSec); as.tick(clk.t, d, &saved); CHECK(!saved);   // clean: no further writes
  as.markDirty(clk.t); clk.advance(31 * kNsPerSec); as.tick(clk.t, d, &saved); CHECK(saved && as.saves() == 2);
  // only the show JSON (and its temp/bak) was written: no media paths, no copies of media dirs
  for (const auto& p : ops.writtenPaths) CHECK(p.find("media") == std::string::npos && p.find("auto.json") != std::string::npos);
  // failing save keeps dirty state and retries at the next interval
  FaultFileOps bad(defaultFileOps()); bad.failFlush = true; Autosave as2(bad, t.path("auto2.json"));
  as2.markDirty(clk.t); clk.advance(31 * kNsPerSec);
  CHECK(!as2.tick(clk.t, d, &saved).ok() && !saved && as2.dirty() && !as2.lastError().ok());
  clk.advance(10 * kNsPerSec); as2.tick(clk.t, d, &saved); CHECK(!saved);   // waits another interval
  bad.failFlush = false; clk.advance(25 * kNsPerSec); CHECK(as2.tick(clk.t, d, &saved).ok() && saved && !as2.dirty());
  // autosave never touches the user's own file
  CHECK(!std::filesystem::exists(t.path("show.json")));
}

TEST(persist_crash_recovery_marker_restores_explicit_copy_disarmed) {
  TmpDir t; IFileOps& ops = defaultFileOps();
  CrashRecovery cr(ops, t.path("state"));
  CHECK(!cr.detect().crashed);
  CHECK(cr.beginSession(1000).ok());
  ShowDocument d = richDoc(); d.name = "Gig In Progress";
  Autosave as(ops, cr.autosavePath(), 30 * kNsPerSec); as.markDirty(0); CHECK(as.tick(31 * kNsPerSec, d).ok());
  // --- process "crashes": no endSession ---
  CrashRecovery cr2(ops, t.path("state"));
  RecoveryInfo ri = cr2.detect();
  CHECK(ri.crashed && ri.autosaveValid && ri.showName == "Gig In Progress" && ri.markerWallMs == 1000);
  ShowIO io; CHECK(io.save(t.path("mine.json"), richDoc()).ok());
  std::string original = slurp(t.path("mine.json"));
  RestoredShow rs; CHECK(cr2.restoreCopy(t.path("mine.json"), rs).ok());
  CHECK(rs.savedPath != t.path("mine.json") && rs.savedPath.find("recovered") != std::string::npos);
  CHECK(slurp(t.path("mine.json")) == original);                      // explicit copy; original never overwritten
  CHECK(rs.startDisarmed && rs.startIdle && rs.doc.name == "Gig In Progress");
  ShowEngine eng(EngineOptions{true, 8, 2500}); CHECK(eng.replaceDocument(rs.doc).ok());
  CHECK(!eng.snapshot().lightingArmRequested && eng.snapshot().idleActive);
  // corrupted autosave is reported as not valid
  write(cr2.autosavePath(), "junk"); std::remove(ShowIO::backupPath(cr2.autosavePath(), 1).c_str());
  CHECK(!cr2.detect().autosaveValid);
  // clean shutdown removes the marker
  CHECK(cr2.endSession().ok() && !cr2.detect().crashed);
  CHECK(cr2.beginSession(5).ok()); CHECK(cr2.discard().ok() && !cr2.detect().crashed);
}

TEST(persist_local_settings_secret_refs_only) {
  TmpDir t; IFileOps& ops = defaultFileOps();
  LocalSettings s;
  s.devices["audioInput"] = "{0.0.1.00000000}.{abcd}"; s.outputDisplays["main"] = "DISPLAY\\ABC1234\\5&1a2b"; s.secretRefs["youtube"] = "dpapi:stagesignal/youtube-1"; s.recordingFolder = "D:/rec";
  CHECK(saveSettings(ops, t.path("settings.json"), s).ok());
  LocalSettings back; CHECK(loadSettings(ops, t.path("settings.json"), back).ok());
  CHECK(back.devices == s.devices && back.secretRefs == s.secretRefs && back.outputDisplays == s.outputDisplays && back.recordingFolder == "D:/rec");
  LocalSettings bad = s; bad.secretRefs["youtube"] = "rtmps://a.rtmp.youtube.com/live2/abcd-efgh-ijkl-mnop"; CHECK(!saveSettings(ops, t.path("bad.json"), bad).ok());
  bad.secretRefs["youtube"] = "p@ss word"; CHECK(!validateSettings(bad).ok());
  CHECK(!std::filesystem::exists(t.path("bad.json")));
  std::string txt = slurp(t.path("settings.json")); txt[txt.find("D:/rec")] = 'E'; write(t.path("tamper.json"), txt);
  CHECK(!loadSettings(ops, t.path("tamper.json"), back).ok());
  // settings never live in the show file
  std::string showText = encodeShow(richDoc());
  CHECK(showText.find("audioInput") == std::string::npos && showText.find("secretRefs") == std::string::npos);
}

TEST(persist_hash_and_inflate_vectors) {
  CHECK(Sha256::hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(Sha256::hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  std::string m(1000, 'a'); Sha256 s; s.update(m.data(), 333); s.update(m.data() + 333, 667);
  CHECK(s.finalHex() == Sha256::hex(m.data(), m.size()));
  CHECK(crc32("123456789", 9) == 0xCBF43926u);
}

int main() { return ss_test::runAll(); }
