#include "test.h"
#include "diagnostics/event_journal.h"
#include "nlohmann/json.hpp"
#include <cmath>
using namespace ss;
static bool has(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }

TEST(diag_journal_timestamps_both_clocks) {
  FakeClock clk; clk.t = 123456789; clk.wall = 1700000000000LL; EventJournal j(clk);
  j.record(JournalKind::Scene, "scene-3", "crossfade"); clk.advance(kNsPerSec);
  j.marker("chorus");
  auto v = j.snapshot(); CHECK(v.size() == 2);
  CHECK(v[0].monoNs == 123456789 && v[0].wallMs == 1700000000000LL);
  CHECK(v[1].monoNs == 123456789 + kNsPerSec && v[1].wallMs == 1700000001000LL);
  CHECK(v[0].seq == 1 && v[1].seq == 2 && v[1].kind == JournalKind::Marker);
  auto o = nlohmann::json::parse(EventJournal::toJsonLine(v[1]));
  CHECK(o["mono_ns"] == 123456789 + kNsPerSec); CHECK(o["wall_ms"] == 1700000001000LL); CHECK(o["kind"] == "marker"); CHECK(o["subject"] == "chorus");
  CHECK(o["wall_iso"] == "2023-11-14T22:13:21.000Z");
}
TEST(diag_journal_ring_bounded) {
  FakeClock clk; JournalConfig c; c.ringCapacity = 16; EventJournal j(clk, c);
  for (int i = 0; i < 1000; ++i) j.record(JournalKind::Cue, "cue-" + std::to_string(i));
  CHECK(j.size() == 16); auto v = j.snapshot(); CHECK(v.front().subject == "cue-984" && v.back().subject == "cue-999");
  CHECK(j.stats().evictedFromRing == 984 && j.stats().recorded == 1000);
}
TEST(diag_journal_tempo_key_max_1hz_and_unknown) {
  FakeClock clk; EventJournal j(clk); Estimate t, k;
  t.valid = true; t.value = 128.5; t.confidence = 0.8f; k.valid = false;
  int accepted = 0;
  for (int i = 0; i < 100; ++i) { if (j.recordTempoKey(t, k)) ++accepted; clk.advance(100 * kNsPerMs); }   // 10 s at 10 Hz
  CHECK(accepted == 10); CHECK(j.stats().tempoKeyThrottled == 90);
  auto v = j.snapshot(); CHECK(has(v[0].detail, "bpm=128.5")); CHECK(has(v[0].detail, "tonality=unknown")); CHECK(std::isnan(v[0].b));
  auto o = nlohmann::json::parse(EventJournal::toJsonLine(v[0])); CHECK(o["b"].is_null()); CHECK(o["a"] == 128.5);
  Estimate k2; k2.valid = true; k2.value = 9; k2.minor = true; k2.confidence = 0.5f; clk.advance(2 * kNsPerSec);
  CHECK(j.recordTempoKey(Estimate(), k2)); CHECK(has(j.snapshot().back().detail, "bpm=unknown")); CHECK(has(j.snapshot().back().detail, "tonality=A minor"));
}
TEST(diag_journal_no_keys_and_bounded_strings) {
  FakeClock clk; EventJournal j(clk); sec::Redactor red; red.addLiteral("SUPERSECRETKEY"); j.setRedactor(&red);
  j.record(JournalKind::Broadcast, "start rtmps://u:pw1@live.example/app/abcdefgh12345678?k=1", "failed: stream_key=SUPERSECRETKEY and SUPERSECRETKEY again C:\\Users\\bob\\x");
  j.record(JournalKind::Device, std::string(5000, 's'), std::string(5000, 'd'));
  auto v = j.snapshot();
  CHECK(!has(v[0].subject, "pw1")); CHECK(!has(v[0].subject, "abcdefgh")); CHECK(!has(v[0].detail, "SUPERSECRET")); CHECK(!has(v[0].detail, "bob"));
  CHECK(v[1].subject.size() <= 96 && v[1].detail.size() <= 256);
  CHECK(!has(EventJournal::toJsonLine(v[0]), "SUPERSECRET"));
}
TEST(diag_journal_jsonl_writer_and_rotation) {
  FakeClock clk; MemFileOps fs; RotationConfig rc; rc.dir = "j"; rc.baseName = "journal"; rc.ext = ".jsonl"; rc.maxBytes = 2000; rc.maxFiles = 3; RotatingFile rf(fs, rc);
  EventJournal j(clk); j.attachFile(&rf);
  for (int i = 0; i < 100; ++i) { j.record(JournalKind::Scene, "s" + std::to_string(i), "detail"); clk.advance(kNsPerMs); }
  CHECK(fs.files.empty());                          // nothing written until flush (record() never touches disk)
  CHECK(j.flush() == 100);
  CHECK(rf.existingPaths().size() <= 3); CHECK(rf.totalBytes() <= 6000);
  std::string last; fs.readAll("j/journal.jsonl", last, 10000);
  size_t pos = 0; int n = 0; while (pos < last.size()) { size_t e = last.find('\n', pos); auto o = nlohmann::json::parse(last.substr(pos, e - pos)); CHECK(o.contains("mono_ns") && o.contains("wall_ms")); pos = e + 1; ++n; }
  CHECK(n > 0);
}
TEST(diag_journal_pending_bounded_when_not_flushed) {
  FakeClock clk; MemFileOps fs; RotationConfig rc; rc.dir = "j"; rc.baseName = "journal"; rc.ext = ".jsonl"; RotatingFile rf(fs, rc);
  JournalConfig c; c.pendingCapacity = 50; c.ringCapacity = 10; EventJournal j(clk, c); j.attachFile(&rf);
  for (int i = 0; i < 500; ++i) j.record(JournalKind::Cue, "c");
  CHECK(j.stats().droppedPending == 450);
  CHECK(j.flush() == 51);                           // 50 events + 1 synthetic overflow note
  std::string s; fs.readAll("j/journal.jsonl", s, 100000); CHECK(has(s, "events dropped=450"));
}
TEST(diag_journal_write_failure_counted_not_fatal) {
  FakeClock clk; MemFileOps fs; fs.failAll = true; RotationConfig rc; rc.dir = "j"; RotatingFile rf(fs, rc); EventJournal j(clk); j.attachFile(&rf);
  j.record(JournalKind::Note, "x"); CHECK(j.flush() == 0); CHECK(j.stats().writeFailures == 1); CHECK(j.size() == 1);
}
TEST(diag_journal_all_kinds_named) {
  for (int k = 0; k <= (int)JournalKind::Note; ++k) CHECK(std::string(journalKindName((JournalKind)k)) != "?");
}
int main() { return ss_test::runAll(); }
