#include "test.h"
#include "diagnostics/logger.h"
#include "diagnostics/retention.h"
using namespace ss;

static bool has(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }

TEST(diag_rotation_5x5) {
  MemFileOps fs; RotationConfig rc; rc.dir = "logs";                // defaults: 5 files x 5 MiB
  CHECK(rc.maxBytes == 5u * 1024 * 1024 && rc.maxFiles == 5);
  rc.maxBytes = 1000;
  RotatingFile rf(fs, rc);
  std::string line(99, 'x'); line += '\n';
  for (int i = 0; i < 200; ++i) CHECK(rf.append(line));
  // at most 5 files, each <= 1000 bytes, total bounded
  CHECK(rf.existingPaths().size() == 5);
  for (auto& p : rf.existingPaths()) CHECK(fs.fileSize(p) <= 1000 && fs.fileSize(p) > 0);
  CHECK(fs.files.size() == 5); CHECK(rf.totalBytes() <= 5000);
  CHECK(rf.rotations() >= 19);
  CHECK(rf.pathFor(0) == "logs/stagesignal.log" && rf.pathFor(4) == "logs/stagesignal.4.log");
  CHECK(fs.files.count("logs/stagesignal.5.log") == 0);
  // oversized record is truncated to the cap, never exceeds a file
  std::string huge(5000, 'y'); CHECK(rf.append(huge)); CHECK(rf.truncatedRecords() == 1);
  for (auto& p : rf.existingPaths()) CHECK(fs.fileSize(p) <= 1000);
}
TEST(diag_rotation_order_and_content) {
  MemFileOps fs; RotationConfig rc; rc.dir = "d"; rc.maxBytes = 64; rc.maxFiles = 3; RotatingFile rf(fs, rc);
  for (int i = 0; i < 10; ++i) rf.append("line-" + std::to_string(i) + "-aaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");   // ~34 bytes -> 1 per file after rotate
  auto ps = rf.existingPaths(); CHECK(ps.size() == 3);
  std::string a, b, c; fs.readAll(ps[0], a, 1000); fs.readAll(ps[2], c, 1000);
  CHECK(has(a, "line-7") || has(a, "line-8")); CHECK(has(c, "line-9"));        // oldest first, newest last
  CHECK(!has(a, "line-0"));
}
TEST(diag_rotation_disk_full_counts_and_recovers) {
  MemFileOps fs; fs.quotaBytes = 50; RotationConfig rc; rc.dir = "d"; RotatingFile rf(fs, rc);
  CHECK(rf.append("0123456789\n")); CHECK(!rf.append(std::string(100, 'z') + "\n")); CHECK(rf.failures() == 1);
  fs.quotaBytes = UINT64_MAX; CHECK(rf.append("ok\n"));
}
TEST(diag_logger_sanitizes_and_formats) {
  FakeClock clk; clk.wall = 1700000000123LL; clk.t = 5 * kNsPerSec;
  MemFileOps fs; RotationConfig rc; rc.dir = "d"; RotatingFile rf(fs, rc); Logger lg(clk, rf);
  sec::Redactor red; red.addLiteral("MyStreamKey-9876"); lg.setRedactor(&red);
  lg.info("broadcast", "connecting rtmps://u:p4ss@live.example/app/abcdef1234567890?token=Z\nmultiline C:\\Users\\Al\\x and MyStreamKey-9876 stream_key=abc");
  lg.debug("x", "filtered");
  lg.error("we ird/comp", "boom");   // Error flushes immediately
  std::string out; fs.readAll("d/stagesignal.log", out, 1 << 20);
  CHECK(has(out, "2023-11-14T22:13:20.123Z +5000ms INFO broadcast connecting"));
  CHECK(!has(out, "p4ss")); CHECK(!has(out, "abcdef12")); CHECK(!has(out, "Al\\")); CHECK(!has(out, "MyStreamKey")); CHECK(!has(out, "=abc"));
  CHECK(has(out, "ERROR we_ird_comp boom")); CHECK(!has(out, "filtered"));
  size_t nl = 0; for (char c : out) nl += (c == '\n'); CHECK(nl == 2);       // newline in message did not split the record
  CHECK(lg.stats().lines == 2 && lg.stats().filteredByLevel == 1);
}
TEST(diag_logger_bounded_buffer_when_disk_fails) {
  FakeClock clk; MemFileOps fs; fs.failAll = true; RotationConfig rc; rc.dir = "d"; RotatingFile rf(fs, rc);
  LoggerConfig lc; lc.bufferBytes = 512; Logger lg(clk, rf, lc);
  for (int i = 0; i < 5000; ++i) lg.info("c", "message number " + std::to_string(i));
  CHECK(lg.stats().flushFailures > 0); CHECK(lg.stats().bytesDroppedOnFailure > 0);
  fs.failAll = false; lg.info("c", "after recovery"); lg.flush();
  std::string out; CHECK(fs.readAll("d/stagesignal.log", out, 1 << 20)); CHECK(has(out, "after recovery"));
}
TEST(diag_logger_line_cap) {
  FakeClock clk; MemFileOps fs; RotationConfig rc; rc.dir = "d"; RotatingFile rf(fs, rc); Logger lg(clk, rf);
  lg.warn("c", std::string(100000, 'q'));
  CHECK(fs.fileSize("d/stagesignal.log") < 1300);
}
TEST(diag_retention_and_delete_controls) {
  FakeClock clk; clk.wall = 100LL * 86400000; MemFileOps fs; fs.nowMs = clk.wall - 40LL * 86400000;
  RotationConfig rc; rc.dir = "d"; rc.maxBytes = 100; rc.maxFiles = 3; RotatingFile rf(fs, rc), jf(fs, [] { RotationConfig j; j.dir = "j"; j.baseName = "journal"; j.ext = ".jsonl"; return j; }());
  Logger lg(clk, rf); EventJournal jr(clk); jr.attachFile(&jf);
  lg.error("c", std::string(80, 'a')); lg.error("c", std::string(80, 'b'));             // old (40 d) files
  fs.nowMs = clk.wall - 1LL * 86400000; lg.error("c", std::string(80, 'c'));            // recent file
  jr.record(JournalKind::Note, "n"); jr.flush();
  DiagnosticsRetention ret(clk); ret.setLogger(&lg); ret.setJournal(&jr, &jf); ret.setPolicy({30});
  size_t before = fs.files.size(); size_t removed = ret.enforce();
  CHECK(removed >= 1 && fs.files.size() == before - removed);
  std::string cur; CHECK(fs.readAll("d/stagesignal.log", cur, 10000) && has(cur, "ccc"));
  CHECK(ret.deleteAll() >= 1); CHECK(fs.files.empty()); CHECK(jr.size() == 0);
  ret.setPolicy({0}); CHECK(ret.enforce() == 0);
}
int main() { return ss_test::runAll(); }
