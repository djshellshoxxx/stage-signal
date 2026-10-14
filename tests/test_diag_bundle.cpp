#include "test.h"
#include "diagnostics/support_bundle.h"
#include "diagnostics/zip_store.h"
#include "diagnostics/logger.h"
#include "nlohmann/json.hpp"
#include <map>
using namespace ss;
static bool has(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }
static uint32_t rd16(const std::string& s, size_t o) { return (unsigned char)s[o] | ((unsigned char)s[o + 1] << 8); }
static uint32_t rd32(const std::string& s, size_t o) { return rd16(s, o) | (rd16(s, o + 2) << 16); }

// Minimal reader: walks the central directory, verifies STORE + CRC, returns name -> data.
static bool readZip(const std::string& z, std::map<std::string, std::string>& out) {
  if (z.size() < 22) return false;
  size_t e = z.size() - 22; if (rd32(z, e) != 0x06054b50) return false;
  uint32_t n = rd16(z, e + 10), cdOff = rd32(z, e + 16), cdSize = rd32(z, e + 12); if (cdOff + cdSize != e) return false;
  size_t p = cdOff;
  for (uint32_t i = 0; i < n; ++i) {
    if (rd32(z, p) != 0x02014b50) return false;
    uint32_t method = rd16(z, p + 10), crc = rd32(z, p + 16), csz = rd32(z, p + 20), usz = rd32(z, p + 24), nl = rd16(z, p + 28), lo = rd32(z, p + 42);
    std::string name = z.substr(p + 46, nl); if (method != 0 || csz != usz) return false;
    if (rd32(z, lo) != 0x04034b50) return false;
    size_t ds = lo + 30 + rd16(z, lo + 26) + rd16(z, lo + 28); std::string data = z.substr(ds, usz);
    if (crc32(data.data(), data.size()) != crc) return false;
    out[name] = data; p += 46 + nl;
  }
  return true;
}

static const char* kSecrets[] = {"hunter2pw", "SK_abcdef0123456789", "alice_the_user", "tok_9f8e7d6c5b4a", "MyLiteralKey-42", "p%40ss", "sekrit-passphrase"};

struct Fixture {
  FakeClock clk; MemFileOps fs; RotationConfig lrc, jrc; std::unique_ptr<RotatingFile> logs, jfile; std::unique_ptr<EventJournal> journal; sec::Redactor red;
  Fixture() {
    clk.wall = 1700000000000LL; clk.t = 42 * kNsPerSec; fs.nowMs = clk.wall;
    lrc.dir = "diag/logs"; jrc.dir = "diag/journal"; jrc.baseName = "journal"; jrc.ext = ".jsonl";
    logs.reset(new RotatingFile(fs, lrc)); jfile.reset(new RotatingFile(fs, jrc)); journal.reset(new EventJournal(clk)); journal->attachFile(jfile.get());
    red.addLiteral("MyLiteralKey-42");
    // Hostile, UNSANITIZED legacy content written straight to disk (bundle must defend itself).
    std::string raw;
    raw += "2023 connect rtmps://bob:hunter2pw@live.example.com/app/SK_abcdef0123456789?token=tok_9f8e7d6c5b4a\n";
    raw += "open C:\\Users\\alice_the_user\\Documents\\show.json\n";
    raw += "cfg {\"password\":\"sekrit-passphrase\",\"x\":1} MyLiteralKey-42 url=rtmps://h/app/k?pw=p%40ss\n";
    raw += std::string("nul\0byte secret=hunter2pw\n", 26);
    fs.writeAll("diag/logs/stagesignal.log", raw);
    fs.writeAll("diag/logs/stagesignal.1.log", "old line apiKey=tok_9f8e7d6c5b4a\n");
    fs.writeAll("diag/journal/journal.jsonl", "{\"seq\":1,\"kind\":\"note\",\"subject\":\"rtmps://a:hunter2pw@h/x/SK_abcdef0123456789\",\"detail\":\"d\"}\nnot json stream_key=hunter2pw\n");
    journal->record(JournalKind::Marker, "chorus"); journal->record(JournalKind::Broadcast, "live", "key=SK_abcdef0123456789");
  }
};

static bool leaks(const std::string& s) { for (auto k : kSecrets) if (has(s, k)) return true; return false; }

TEST(diag_bundle_preview_required_and_validated) {
  Fixture f; SupportBundle b(f.fs, f.clk); b.addLogSource(f.logs.get()); b.setJournal(f.journal.get(), f.jfile.get()); b.setSystemInfo({{"os", "Windows 7"}});
  CHECK(!b.exportZip("out/b.zip", {"logs"}).ok());                      // no preview yet
  auto cats = b.preview();
  CHECK(cats.size() == 3 && cats[0].id == "logs" && cats[0].files == 2 && cats[0].approxBytes > 100);
  CHECK(cats[1].id == "journal" && cats[2].id == "system");
  for (auto& c : cats) CHECK(!c.title.empty() && !c.description.empty());
  CHECK(!b.exportZip("out/b.zip", {"logs", "camera"}).ok());           // unknown category
  CHECK(!b.exportZip("out/b.zip", {}).ok());                           // nothing approved
  CHECK(f.fs.fileSize("out/b.zip") < 0);
  CHECK(SupportBundle::alwaysExcluded().size() >= 5);
}
TEST(diag_bundle_zip_contents_have_no_secrets) {
  Fixture f; SupportBundle b(f.fs, f.clk); b.setRedactor(&f.red); b.addLogSource(f.logs.get()); b.setJournal(f.journal.get(), f.jfile.get());
  b.setSystemInfo({{"os", "Windows 7 SP1"}, {"cpu", "Pentium III 800 MHz"}, {"profile_path", "C:\\Users\\alice_the_user\\AppData"}, {"stream_key", "hunter2pw"}, {"url", "rtmps://bob:hunter2pw@h/a/SK_abcdef0123456789"}});
  CHECK(b.addTextSection("benchmark", "Benchmark summary", "bench.txt", "BEN-01 ok\nendpoint rtmps://u:hunter2pw@h/live/SK_abcdef0123456789\n").ok());
  auto cats = b.preview(); CHECK(cats.size() == 4);
  BundleReport rep; Status s = b.exportZip("out/support.zip", {"logs", "journal", "system", "benchmark"}, &rep);
  CHECK(s.ok()); if (!s.ok()) std::printf("  %s\n", s.detail.c_str());
  CHECK(f.fs.fileSize("out/support.zip.part") < 0); CHECK(f.fs.fileSize("out/support.zip") > 0);
  std::string z; CHECK(f.fs.readAll("out/support.zip", z, 1 << 24)); std::map<std::string, std::string> m; CHECK(readZip(z, m));
  CHECK(m.count("manifest.json") && m.count("system/system.json") && m.count("logs/stagesignal.log") && m.count("logs/stagesignal.1.log") && m.count("journal/journal.jsonl") && m.count("extra/bench.txt"));
  CHECK(m.size() == rep.files);
  for (auto& kv : m) { if (leaks(kv.second)) { std::printf("LEAK in %s\n", kv.first.c_str()); for (auto k : kSecrets) if (has(kv.second, k)) std::printf("  secret %s\n%s\n", k, kv.second.c_str()); } CHECK(!leaks(kv.second)); CHECK(!SupportBundle::isExcludedArtifactName(kv.first)); CHECK(kv.second.find('\0') == std::string::npos); }
  CHECK(!leaks(z));                                              // nothing leaks even in raw archive bytes (STORE => visible)
  CHECK(has(m["logs/stagesignal.log"], "live.example.com"));       // useful non-secret context kept
  CHECK(has(m["logs/stagesignal.log"], "<user>"));
  CHECK(has(m["system/system.json"], "Pentium III"));
  // journal lines remain valid JSON, ring events included when file already has them
  std::string jr = m["journal/journal.jsonl"]; size_t pos = 0; int lines = 0;
  while (pos < jr.size()) { size_t e = jr.find('\n', pos); auto o = nlohmann::json::parse(jr.substr(pos, e - pos)); CHECK(o.is_object()); pos = e + 1; ++lines; }
  CHECK(lines >= 4);
  auto man = nlohmann::json::parse(m["manifest.json"]); CHECK(man["excluded"].size() >= 5); CHECK(man["files"].size() == m.size() - 1);
  CHECK(man["generated_wall_ms"] == 1700000000000LL);
  for (auto& fe : man["files"]) CHECK(fe["crc32"].get<uint32_t>() == crc32(m[fe["name"].get<std::string>()].data(), m[fe["name"].get<std::string>()].size()));
  // existing destination is never overwritten
  CHECK(!b.exportZip("out/support.zip", {"logs"}).ok());
}
TEST(diag_bundle_only_approved_categories) {
  Fixture f; SupportBundle b(f.fs, f.clk); b.addLogSource(f.logs.get()); b.setJournal(f.journal.get(), f.jfile.get()); b.setSystemInfo({{"os", "x"}});
  b.preview(); CHECK(b.exportZip("o/a.zip", {"system"}).ok());
  std::string z; f.fs.readAll("o/a.zip", z, 1 << 24); std::map<std::string, std::string> m; CHECK(readZip(z, m));
  CHECK(m.size() == 2 && m.count("system/system.json") && m.count("manifest.json"));
}
TEST(diag_bundle_folder_export_and_failure_cleanup) {
  Fixture f; SupportBundle b(f.fs, f.clk); b.setRedactor(&f.red); b.addLogSource(f.logs.get()); b.preview();
  CHECK(b.exportFolder("out/dir", {"logs"}).ok());
  std::string l; CHECK(f.fs.readAll("out/dir/logs/stagesignal.log", l, 1 << 20)); CHECK(!leaks(l));
  CHECK(!b.exportFolder("out/dir", {"logs"}).ok());                  // non-empty destination
  Fixture g; SupportBundle b2(g.fs, g.clk); b2.addLogSource(g.logs.get()); b2.preview();
  g.fs.quotaBytes = g.fs.totalBytes() + 10;                           // disk full mid-export
  CHECK(!b2.exportZip("o/z.zip", {"logs"}).ok()); CHECK(g.fs.fileSize("o/z.zip") < 0); CHECK(g.fs.fileSize("o/z.zip.part") < 0);
}
TEST(diag_bundle_never_includes_excluded_artifacts) {
  const char* bad[] = {"a.wav", "x/cam.PNG", "core.dmp", "crash.txt", "streamkey.txt", "my.pem", "show.stagesignal", "clip.mp4", "Secrets.json", "a.zip"};
  for (auto n : bad) CHECK(SupportBundle::isExcludedArtifactName(n));
  CHECK(!SupportBundle::isExcludedArtifactName("logs/stagesignal.log"));
  FakeClock clk; MemFileOps fs; SupportBundle b(fs, clk);
  CHECK(!b.addTextSection("x", "t", "audio.wav", "RIFF").ok()); CHECK(!b.addTextSection("x", "t", "../x.txt", "").ok());
  CHECK(!b.addTextSection("logs", "t", "ok.txt", "").ok()); CHECK(!b.addTextSection("x", "t", "dump.dmp", "").ok());
  CHECK(!b.addTextSection("X Y", "t", "ok.txt", "").ok()); CHECK(!b.addTextSection("x", "t", "noext", "").ok());
  CHECK(b.addTextSection("x", "t", "ok.txt", "hello").ok());
  b.preview(); CHECK(b.exportZip("p/..", {"x"}).code == Err::InvalidArgument);   // unsafe destination name
}
TEST(diag_bundle_oversize_log_skipped_and_reported) {
  Fixture f; BundleLimits lim; lim.maxFileBytes = 100; SupportBundle b(f.fs, f.clk, lim); b.addLogSource(f.logs.get()); b.preview();
  CHECK(b.exportZip("o/s.zip", {"logs"}).ok());
  std::string z; f.fs.readAll("o/s.zip", z, 1 << 24); std::map<std::string, std::string> m; CHECK(readZip(z, m));
  auto man = nlohmann::json::parse(m["manifest.json"]); CHECK(man["skipped"].size() >= 1);
}
TEST(diag_zip_store_roundtrip_and_limits) {
  std::string out; ZipStoreWriter w([&](const char* p, size_t n) { out.append(p, n); return true; });
  CHECK(w.addFile("a.txt", "hello", 1700000000000LL).ok()); CHECK(w.addFile("d/b.bin", std::string(70000, 'x'), 0).ok());
  CHECK(!w.addFile("../evil.txt", "x", 0).ok()); CHECK(!w.addFile("/abs.txt", "x", 0).ok()); CHECK(!w.addFile("C:\\x", "x", 0).ok());
  CHECK(w.finish().ok()); CHECK(!w.addFile("late.txt", "x", 0).ok());
  std::map<std::string, std::string> m; CHECK(readZip(out, m)); CHECK(m.size() == 2 && m["a.txt"] == "hello" && m["d/b.bin"].size() == 70000);
  CHECK(crc32("123456789", 9) == 0xCBF43926u);
  ZipStoreWriter bad([](const char*, size_t) { return false; }); CHECK(bad.addFile("a", "b", 0).code == Err::DiskFull);
}
int main() { return ss_test::runAll(); }
