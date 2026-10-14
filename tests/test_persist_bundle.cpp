#include "persistence/bundle.h"
#include "persistence/hash.h"
#include "persistence/show_io.h"
#include "persistence/zip.h"
#include "nlohmann/json.hpp"
#include "test_persist_fixtures.h"
#include "test_show_util.h"
#include <fstream>
using namespace sst;
using namespace ss::persist;

// ---- raw ZIP builder able to produce hostile archives ----------------------------------------------------
struct RZ {
  std::string name, data;                 // data is stored verbatim (already deflated if method 8)
  uint16_t method = 0, flags = 0, madeBy = 20; uint32_t extAttr = 0;
  int64_t usize = -1, csize = -1;         // override declared sizes (-1 = actual)
  int64_t crc = -1;                       // override CRC (-1 = crc of data for stored; for deflate pass explicit)
};
static void p16(std::string& s, uint32_t v) { s.push_back((char)v); s.push_back((char)(v >> 8)); }
static void p32(std::string& s, uint32_t v) { p16(s, v & 0xFFFF); p16(s, v >> 16); }
static std::string buildZip(const std::vector<RZ>& es, int eocdCountOverride = -1) {
  std::string out, cd;
  for (const RZ& e : es) {
    uint32_t off = (uint32_t)out.size();
    uint32_t crc = e.crc >= 0 ? (uint32_t)e.crc : crc32(e.data.data(), e.data.size());
    uint32_t cs = e.csize >= 0 ? (uint32_t)e.csize : (uint32_t)e.data.size(), us = e.usize >= 0 ? (uint32_t)e.usize : (uint32_t)e.data.size();
    p32(out, 0x04034b50); p16(out, 20); p16(out, e.flags); p16(out, e.method); p16(out, 0); p16(out, 0x21);
    p32(out, crc); p32(out, cs); p32(out, us); p16(out, (uint32_t)e.name.size()); p16(out, 0); out += e.name; out += e.data;
    p32(cd, 0x02014b50); p16(cd, e.madeBy); p16(cd, 20); p16(cd, e.flags); p16(cd, e.method); p16(cd, 0); p16(cd, 0x21);
    p32(cd, crc); p32(cd, cs); p32(cd, us); p16(cd, (uint32_t)e.name.size()); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, e.extAttr); p32(cd, off); cd += e.name;
  }
  uint32_t cdOff = (uint32_t)out.size();
  out += cd;
  uint32_t n = eocdCountOverride >= 0 ? (uint32_t)eocdCountOverride : (uint32_t)es.size();
  p32(out, 0x06054b50); p16(out, 0); p16(out, 0); p16(out, n); p16(out, n); p32(out, (uint32_t)cd.size()); p32(out, cdOff); p16(out, 0);
  return out;
}
static std::string storedDeflate(const std::string& d) {          // single stored deflate block (<=65535)
  std::string s; s.push_back(1); p16(s, (uint32_t)d.size()); p16(s, (uint32_t)~d.size() & 0xFFFF); return s + d;
}
static std::string bytes(const unsigned char* p, size_t n) { return std::string((const char*)p, n); }
static void put(const std::string& path, const std::string& d) { std::ofstream(path, std::ios::binary) << d; }
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), {}); }

struct Fixture {
  TmpDir src;                                // media root
  ShowDocument doc;
  std::vector<std::string> contents;
  Fixture() {
    doc = baseDoc(3);
    doc.texts = {{"t", "Hello"}};
    std::string c0 = "PNGDATA-0", c1(300000, 'q'), c2;
    for (int i = 0; i < 5000; ++i) c2 += (char)(i * 31 + 7);
    contents = {c0, c1, c2};
    const char* fmts[] = {"png", "gif", "mp4"};
    for (int i = 0; i < 3; ++i) {
      MediaRecord m; m.hash = Sha256::hex(contents[(size_t)i].data(), contents[(size_t)i].size()); m.format = fmts[i];
      m.relPath = std::string("assets/clip") + std::to_string(i) + "." + fmts[i]; m.bytes = contents[(size_t)i].size();
      std::filesystem::create_directories(src.path("assets"));
      put(src.path(m.relPath), contents[(size_t)i]);
      doc.media.push_back(m);
    }
    doc.cues = {msCue("c0", "s0", 1000)}; doc.cues[0].mediaRef = doc.media[0].hash;
    doc.midiMappings = {"{\"action\":\"NextScene\",\"deviceId\":\"USB\\\\VID_1234&PID_5678\\\\SERIAL99\",\"note\":60}",
                        "{\"action\":\"Brightness\",\"nested\":{\"portId\":\"{guid}\",\"streamKey\":\"SUPERSECRETKEY\",\"cc\":7}}"};
    doc.patch = {"{\"profile\":\"d\",\"addr\":1,\"secretRef\":\"dpapi:abc\"}"};
    doc.journal.push_back({1, "marker", "{\"note\":\"private\"}"});
    doc.outputs[0].displayHint = "Projector left";
  }
};
static bool noStaging(const std::string& parent) {
  std::vector<std::string> n; defaultFileOps().listDir(parent, n);
  for (auto& x : n) if (x.find(".ss_import_") == 0) return false;
  return true;
}

TEST(bundle_export_import_roundtrip_relocated_offline) {
  Fixture fx; TmpDir out; IFileOps& ops = defaultFileOps();
  ExportOptions eo; eo.mediaRoot = fx.src.path();
  ExportReport rep;
  Status s = exportBundle(ops, fx.doc, eo, out.path("gig.stagesignal"), &rep);
  CHECK(s.ok());
  CHECK(rep.mediaIncluded == 3 && rep.missingMedia.empty() && rep.archiveBytes > 300000);
  CHECK(!std::filesystem::exists(out.path("gig.stagesignal.tmp")));
  // physical ids / secrets / journal removed from the export, reported in the preview
  bool devGone = false, keyGone = false, refGone = false, jGone = false;
  for (auto& r : rep.removedFields) { if (r.find("deviceId") != std::string::npos) devGone = true; if (r.find("streamKey") != std::string::npos) keyGone = true; if (r.find("secretRef") != std::string::npos) refGone = true; if (r == "journal") jGone = true; }
  CHECK(devGone && keyGone && refGone && jGone);
  std::string raw = slurp(out.path("gig.stagesignal"));
  CHECK(raw.find("SUPERSECRETKEY") == std::string::npos && raw.find("SERIAL99") == std::string::npos && raw.find("dpapi:abc") == std::string::npos && raw.find("private") == std::string::npos);
  CHECK(raw.find("Projector left") != std::string::npos);               // non-secret matching hint kept
  // inspect archive members
  ZipReader zr; CHECK(zr.open(ops, out.path("gig.stagesignal")).ok());
  CHECK(zr.entries().size() == 2 + 3 && zr.find("show.json") >= 0 && zr.find("manifest.json") >= 0);
  CHECK(zr.find("media/" + fx.doc.media[0].hash + ".png") >= 0);
  // import into a fresh location, then relocate the directory elsewhere and load offline
  TmpDir dst;
  ImportResult ir = importBundle(ops, out.path("gig.stagesignal"), dst.path("imported/gig"));
  if (!ir.status.ok()) std::printf("  import failed: %s\n", ir.status.detail.c_str());
  CHECK(ir.status.ok() && ir.mediaComplete && ir.warnings.empty());
  if (!ir.status.ok()) return;
  CHECK(noStaging(dst.path("imported")));
  CHECK(ir.doc.name == fx.doc.name && ir.doc.cues.size() == 1 && ir.doc.scenes.size() == 3 && ir.doc.texts[0].text == "Hello");
  CHECK(ir.doc.midiMappings[0].find("deviceId") == std::string::npos && ir.doc.midiMappings[0].find("NextScene") != std::string::npos);
  for (size_t i = 0; i < 3; ++i) {
    CHECK(ir.doc.media[i].relPath == "media/" + fx.doc.media[i].hash + "." + fx.doc.media[i].format);
    CHECK(slurp(joinPath(ir.mediaRoot, ir.doc.media[i].relPath)) == fx.contents[i]);
  }
  std::filesystem::rename(dst.path("imported/gig"), dst.path("moved"));  // relocate
  ShowIO io; ShowDocument reloaded;
  CHECK(io.load(dst.path("moved/show.json"), reloaded).ok());
  CHECK(slurp(dst.path("moved/" + reloaded.media[1].relPath)) == fx.contents[1]);
  // re-export from the relocated copy gives a loadable bundle again
  ExportOptions eo2; eo2.mediaRoot = dst.path("moved");
  CHECK(exportBundle(ops, reloaded, eo2, dst.path("again.stagesignal")).ok());
  TmpDir d2; CHECK(importBundle(ops, dst.path("again.stagesignal"), d2.path("x")).status.ok());
  // destination collision is refused, nothing overwritten
  CHECK(!importBundle(ops, out.path("gig.stagesignal"), dst.path("moved")).status.ok());
}

TEST(bundle_export_missing_media_and_hash_mismatch) {
  Fixture fx; TmpDir out; IFileOps& ops = defaultFileOps();
  std::filesystem::remove(fx.src.path(fx.doc.media[1].relPath));
  ExportOptions eo; eo.mediaRoot = fx.src.path();
  Status s = exportBundle(ops, fx.doc, eo, out.path("a.stagesignal"));
  CHECK(!s.ok() && s.detail.find("missing") != std::string::npos && !std::filesystem::exists(out.path("a.stagesignal")));
  eo.allowMissingMedia = true; ExportReport rep;
  CHECK(exportBundle(ops, fx.doc, eo, out.path("b.stagesignal"), &rep).ok() && rep.missingMedia.size() == 1 && rep.mediaIncluded == 2);
  TmpDir dst; ImportResult ir = importBundle(ops, out.path("b.stagesignal"), dst.path("b"));
  CHECK(ir.status.ok() && !ir.mediaComplete && ir.warnings.size() == 1 && ir.warnings[0].find("not included") != std::string::npos);
  // no media at all (nonportable-light export)
  eo.includeMedia = false; CHECK(exportBundle(ops, fx.doc, eo, out.path("c.stagesignal")).ok());
  TmpDir d2; CHECK(importBundle(ops, out.path("c.stagesignal"), d2.path("c")).status.ok());
  // tampered source media (bytes no longer match declared hash) is refused
  Fixture f2; put(f2.src.path(f2.doc.media[0].relPath), "tampered");
  ExportOptions e2; e2.mediaRoot = f2.src.path();
  s = exportBundle(ops, f2.doc, e2, out.path("d.stagesignal")); CHECK(!s.ok() && !std::filesystem::exists(out.path("d.stagesignal")));
  // invalid document never exported
  ShowDocument bad = fx.doc; bad.scenes[0].rendererType = "x"; CHECK(!exportBundle(ops, bad, e2, out.path("e.stagesignal")).ok());
  // symlinked media source is not followed
#ifndef _WIN32
  Fixture f3; std::filesystem::remove(f3.src.path(f3.doc.media[0].relPath));
  std::filesystem::create_symlink("/etc/hostname", f3.src.path(f3.doc.media[0].relPath));
  ExportOptions e3; e3.mediaRoot = f3.src.path(); CHECK(!exportBundle(ops, f3.doc, e3, out.path("f.stagesignal")).ok());
#endif
}

// A valid archive skeleton builder that hostile tests mutate.
struct Skel {
  ShowDocument doc; std::string showJson, manifest;
  Skel() { doc = baseDoc(2); showJson = encodeShow(doc); rebuildManifest(); }
  void rebuildManifest() {
    nlohmann::json m = {{"format", "stagesignal.bundle"}, {"version", 1}, {"files", nlohmann::json::array({{{"path", "show.json"}, {"size", showJson.size()}, {"sha256", Sha256::hex(showJson.data(), showJson.size())}}})}, {"media", nlohmann::json::array()}};
    manifest = m.dump();
  }
  std::vector<RZ> base() const { RZ a; a.name = "show.json"; a.data = showJson; RZ b; b.name = "manifest.json"; b.data = manifest; return {a, b}; }
};
static Status importZip(const std::string& zip, TmpDir& dst, const char* name = "out") {
  put(dst.path("in.zip"), zip);
  ImportResult r = importBundle(defaultFileOps(), dst.path("in.zip"), dst.path(name));
  if (!r.status.ok()) { CHECK(!std::filesystem::exists(dst.path(name))); CHECK(noStaging(dst.path())); }   // nothing published, no staging residue
  return r.status;
}

TEST(bundle_hostile_valid_skeleton_imports) {
  Skel k; TmpDir d;
  CHECK(importZip(buildZip(k.base()), d).ok());
  CHECK(std::filesystem::exists(d.path("out/show.json")) && std::filesystem::exists(d.path("out/manifest.json")));
}

TEST(bundle_hostile_paths_rejected) {
  Skel k;
  const char* names[] = {"../evil.txt", "media/../../evil", "/etc/passwd", "C:/Windows/x", "C:\\x", "\\\\server\\share\\x", "..\\x", "a/../b", "./show.json",
                         "media//x", "show.json.", "media/aux.png", "media/nul", "x:stream", "media/\x01x", "caf\xC3\xA9.json", "evil.exe", "media/foo.exe",
                         "media/AAAA.png", "show.json/", "manifest.json/../show.json"};
  for (const char* n : names) {
    TmpDir d; auto z = k.base(); RZ e; e.name = n; e.data = "x"; z.push_back(e);
    Status s = importZip(buildZip(z), d);
    if (!(!s.ok())) std::printf("  accepted hostile name: %s\n", n);
    CHECK(!s.ok());
  }
  // name policy unit checks
  std::string canon;
  CHECK(checkMemberName("media/ab.png", canon).ok() && canon == "media/ab.png");
  CHECK(checkMemberName("Media/AB.PNG", canon).ok() && canon == "media/ab.png");
  CHECK(!checkMemberName("", canon).ok() && !checkMemberName(std::string(300, 'a'), canon).ok());
}

TEST(bundle_hostile_duplicates) {
  Skel k;
  { TmpDir d; auto z = k.base(); z.push_back(z[0]); CHECK(!importZip(buildZip(z), d).ok()); }                      // exact duplicate
  { TmpDir d; auto z = k.base(); RZ e = z[0]; e.name = "SHOW.JSON"; z.push_back(e); CHECK(!importZip(buildZip(z), d).ok()); }   // canonical (case) duplicate
  { TmpDir d; auto z = k.base(); z[0].name = "Show.json"; CHECK(!importZip(buildZip(z), d).ok()); }               // alias for required member
}

TEST(bundle_hostile_symlink_encrypted_zip64_and_structure) {
  Skel k;
  { TmpDir d; auto z = k.base(); RZ e; e.name = "media/" + std::string(64, 'a') + ".png"; e.data = "/etc/passwd"; e.madeBy = (3 << 8) | 20; e.extAttr = (0120777u << 16); z.push_back(e); CHECK(!importZip(buildZip(z), d).ok()); }
  { TmpDir d; auto z = k.base(); z[0].flags = 1; CHECK(!importZip(buildZip(z), d).ok()); }                         // encrypted
  { TmpDir d; auto z = k.base(); z[0].method = 12; CHECK(!importZip(buildZip(z), d).ok()); }                        // bzip2 unsupported
  { TmpDir d; auto z = k.base(); z[0].csize = 0xFFFFFFFFll; CHECK(!importZip(buildZip(z), d).ok()); }              // zip64 marker
  { TmpDir d; std::string z = buildZip(k.base()); z.resize(z.size() / 2); CHECK(!importZip(z, d).ok()); }          // truncated
  { TmpDir d; CHECK(!importZip("PK\x03\x04 garbage garbage garbage garbage", d).ok()); }
  { TmpDir d; std::string z = buildZip(k.base()) + "trailing"; CHECK(!importZip(z, d).ok()); }                      // data after EOCD
  { TmpDir d; auto z = k.base(); z[0].crc = 1234; CHECK(!importZip(buildZip(z), d).ok()); }                         // CRC mismatch
  { TmpDir d; auto z = k.base(); z[0].usize = 5; CHECK(!importZip(buildZip(z), d).ok()); }                          // stored size mismatch
  { TmpDir d; auto z = k.base(); CHECK(!importZip(buildZip({z[0]}), d).ok()); }                                      // manifest missing
  { TmpDir d; auto z = k.base(); CHECK(!importZip(buildZip({z[1]}), d).ok()); }                                      // show.json missing
  { TmpDir d; std::string a = buildZip(k.base()); a[0] = 'X'; CHECK(!importZip(a, d).ok()); }                       // damaged local header
}

TEST(bundle_hostile_archive_bombs_and_counts) {
  Skel k; IFileOps& ops = defaultFileOps();
  // too many entries (>10000): rejected from the end record before any entry is touched
  { TmpDir d; std::vector<RZ> z = k.base(); for (int i = 0; i < 10001; ++i) { RZ e; e.name = "media/" + std::string(64, 'a') + ".png"; z.push_back(e); }
    std::string zip = buildZip(z, 10003); put(d.path("a.zip"), zip); ZipReader zr; Status s = zr.open(ops, d.path("a.zip")); CHECK(!s.ok() && s.detail.find("too many entries") != std::string::npos); }
  { TmpDir d; std::vector<RZ> z = k.base(); for (int i = 0; i < 9998; ++i) { RZ e; e.name = "d" + std::to_string(i) + "/"; z.push_back(e); }
    CHECK(!importZip(buildZip(z), d).ok()); }   // count is exactly 10000 but members are not declared ones
  { TmpDir d; std::vector<RZ> z; for (int i = 0; i < 10000; ++i) { RZ e; e.name = "e" + std::to_string(i); z.push_back(e); }
    put(d.path("a.zip"), buildZip(z)); ZipReader zr; CHECK(zr.open(ops, d.path("a.zip")).ok() && zr.entries().size() == 10000); }   // exactly at the limit is structurally fine
  // declared total over 2 GiB (stored sizes are lies; no allocation, rejected at open)
  { TmpDir d; std::vector<RZ> z; for (int i = 0; i < 3; ++i) { RZ e; e.name = "media/" + std::string(63, 'a') + std::to_string(i) + ".png"; e.method = 8; e.data = "x"; e.usize = 900ll * 1024 * 1024; e.csize = 5 * 1024 * 1024; z.push_back(e); }
    put(d.path("a.zip"), buildZip(z)); ZipReader zr; Status s = zr.open(ops, d.path("a.zip")); CHECK(!s.ok()); }
  // compression-ratio bomb with honest sizes: 4 MiB of zeros in ~4 KB
  { TmpDir d; RZ e; e.name = "show.json"; e.method = 8; e.data = bytes(kBombDeflate, sizeof(kBombDeflate)); e.usize = 4 * 1024 * 1024; e.crc = 289882218;
    put(d.path("a.zip"), buildZip({e})); ZipReader zr; Status s = zr.open(ops, d.path("a.zip")); CHECK(!s.ok() && s.detail.find("bomb") != std::string::npos); }
  // lying header: declares 64 KiB but inflates to 4 MiB -> stream is cut at the declared size, never written past it
  { TmpDir d; RZ e; e.name = "show.json"; e.method = 8; e.data = bytes(kBombDeflate, sizeof(kBombDeflate)); e.usize = 65536; e.crc = 0;
    put(d.path("a.zip"), buildZip({e})); ZipReader zr; CHECK(zr.open(ops, d.path("a.zip")).ok());
    size_t total = 0; Status s = zr.extract(0, [&](const uint8_t*, size_t n) { total += n; return true; });
    CHECK(!s.ok() && total <= 65536 + 32768); CHECK(s.code == Err::ResourceExhausted); }
  // JSON member over 8 MiB is refused before its bytes are allocated (even though the archive is structurally fine)
  { TmpDir d; Skel kk; auto z = kk.base(); z[0].data = std::string(9 * 1024 * 1024, ' '); put(d.path("a.zip"), buildZip(z));
    ImportResult r = importBundle(ops, d.path("a.zip"), d.path("o")); CHECK(r.status.code == Err::ResourceExhausted && !std::filesystem::exists(d.path("o"))); }
  // corrupt/truncated deflate data is a clean failure
  { TmpDir d; RZ e; e.name = "show.json"; e.method = 8; e.data = bytes(kDynDeflate, 20); e.usize = 100; e.crc = 0; put(d.path("a.zip"), buildZip({e}));
    ZipReader zr; CHECK(zr.open(ops, d.path("a.zip")).ok()); std::vector<uint8_t> o; CHECK(!zr.extractToMemory(0, 1000, o).ok()); }
}

TEST(bundle_inflate_fixed_dynamic_stored_and_deflated_import) {
  IFileOps& ops = defaultFileOps(); TmpDir d;
  std::string t1 = "hello hello hello hello, stage signal", t2;
  for (int i = 0; i < 300; ++i) t2 += "line " + std::to_string(i) + ": StageSignal show data " + std::to_string(i * i % 97) + "\n";
  RZ a; a.name = "fixed"; a.method = 8; a.data = bytes(kFixedDeflate, sizeof(kFixedDeflate)); a.usize = (int64_t)t1.size(); a.crc = crc32(t1.data(), t1.size());
  RZ b; b.name = "dyn"; b.method = 8; b.data = bytes(kDynDeflate, sizeof(kDynDeflate)); b.usize = (int64_t)t2.size(); b.crc = crc32(t2.data(), t2.size());
  put(d.path("a.zip"), buildZip({a, b}));
  ZipReader zr; CHECK(zr.open(ops, d.path("a.zip")).ok());
  std::vector<uint8_t> o;
  CHECK(zr.extractToMemory((size_t)zr.find("fixed"), 1 << 20, o).ok() && std::string(o.begin(), o.end()) == t1);
  CHECK(zr.extractToMemory((size_t)zr.find("dyn"), 1 << 20, o).ok() && std::string(o.begin(), o.end()) == t2);
  CHECK(zr.extractToMemory((size_t)zr.find("dyn"), 100, o).code == Err::ResourceExhausted);
  // full import from a deflate-method archive (stored deflate blocks)
  Skel k;
  RZ s0; s0.name = "show.json"; s0.method = 8; s0.data = storedDeflate(k.showJson); s0.usize = (int64_t)k.showJson.size(); s0.crc = crc32(k.showJson.data(), k.showJson.size());
  RZ m0; m0.name = "manifest.json"; m0.method = 8; m0.data = storedDeflate(k.manifest); m0.usize = (int64_t)k.manifest.size(); m0.crc = crc32(k.manifest.data(), k.manifest.size());
  put(d.path("b.zip"), buildZip({s0, m0}));
  ImportResult ir = importBundle(ops, d.path("b.zip"), d.path("imp"));
  CHECK(ir.status.ok() && ir.doc.name == "Test Show");
}

TEST(bundle_manifest_and_media_integrity) {
  Fixture fx; TmpDir out; IFileOps& ops = defaultFileOps();
  ExportOptions eo; eo.mediaRoot = fx.src.path();
  Status es = exportBundle(ops, fx.doc, eo, out.path("g.stagesignal")); if (!es.ok()) std::printf("  export: %s\n", es.detail.c_str());
  CHECK(es.ok());
  ZipReader zr; Status os = zr.open(ops, out.path("g.stagesignal")); if (!os.ok()) std::printf("  open: %s\n", os.detail.c_str());
  CHECK(os.ok());
  auto get = [&](const std::string& n) { std::vector<uint8_t> v; zr.extractToMemory((size_t)zr.find(n), 1 << 26, v); return std::string(v.begin(), v.end()); };
  std::string show = get("show.json"), man = get("manifest.json");
  std::string m0name = "media/" + fx.doc.media[0].hash + ".png";
  auto rebuild = [&](std::string showT, std::string manT, std::string mediaData) {
    std::vector<RZ> z; RZ a; a.name = "show.json"; a.data = showT; RZ b; b.name = "manifest.json"; b.data = manT; z = {a, b};
    for (int i = 0; i < 3; ++i) { RZ e; e.name = "media/" + fx.doc.media[(size_t)i].hash + "." + fx.doc.media[(size_t)i].format; e.data = i == 0 ? mediaData : fx.contents[(size_t)i]; z.push_back(e); }
    return buildZip(z);
  };
  { TmpDir d; CHECK(importZip(rebuild(show, man, fx.contents[0]), d).ok()); }                                       // control: accepted
  { TmpDir d; CHECK(!importZip(rebuild(show, man, "PNGDATA-X"), d).ok()); }                                         // media bytes != declared size/hash
  { TmpDir d; CHECK(!importZip(rebuild(show, man, "PNGDATA-Z"), d).ok()); }
  { TmpDir d; std::string alt = show; alt[alt.find("Test Show")] = 'B'; CHECK(!importZip(rebuild(alt, man, fx.contents[0]), d).ok()); }   // show.json != manifest hash / checksum
  { TmpDir d; nlohmann::json m = nlohmann::json::parse(man); m["files"][1]["sha256"] = std::string(64, '0'); CHECK(!importZip(rebuild(show, m.dump(), fx.contents[0]), d).ok()); }
  { TmpDir d; nlohmann::json m = nlohmann::json::parse(man); m["files"].push_back({{"path", "../x"}, {"size", 1}, {"sha256", "00"}}); CHECK(!importZip(rebuild(show, m.dump(), fx.contents[0]), d).ok()); }
  { TmpDir d; CHECK(!importZip(rebuild(show, "{ not json", fx.contents[0]), d).ok()); }
  // media member not referenced by the show
  { TmpDir d; std::vector<RZ> z; RZ a; a.name = "show.json"; a.data = show; RZ b; b.name = "manifest.json"; b.data = man; z = {a, b};
    RZ e; e.name = "media/" + std::string(64, 'e') + ".png"; e.data = "zzz"; z.push_back(e); CHECK(!importZip(buildZip(z), d).ok()); }
  (void)m0name;
}

TEST(bundle_show_with_hostile_document_content_rejected) {
  Skel k;
  auto tryDoc = [&](const std::function<void(nlohmann::json&)>& mut) {
    nlohmann::json env = nlohmann::json::parse(encodeShow(baseDoc(2))); mut(env["show"]);
    std::string c = env["show"].dump(); env["checksum"] = "sha256:" + Sha256::hex(c.data(), c.size());
    Skel kk; kk.showJson = env.dump(); kk.rebuildManifest(); TmpDir d; return importZip(buildZip(kk.base()), d).ok();
  };
  CHECK(tryDoc([](nlohmann::json&) {}));
  CHECK(!tryDoc([](nlohmann::json& s) { s["media"] = nlohmann::json::array({{{"hash", std::string(64, 'a')}, {"relPath", "../../etc/passwd"}, {"format", "png"}, {"bytes", 1}}}); }));
  CHECK(!tryDoc([](nlohmann::json& s) { s["media"] = nlohmann::json::array({{{"hash", std::string(64, 'a')}, {"relPath", "http://evil/x.png"}, {"format", "png"}}}); }));
  CHECK(!tryDoc([](nlohmann::json& s) { s["schema"]["major"] = 7; }));
  CHECK(!tryDoc([](nlohmann::json& s) { s["scenes"][0]["rendererType"] = "shell"; }));
  CHECK(!tryDoc([](nlohmann::json& s) { s["midiMappings"] = nlohmann::json::array({{{"script", "calc.exe"}}}); }));
  CHECK(!tryDoc([](nlohmann::json& s) { s["texts"] = nlohmann::json::array({{{"id", "a"}, {"text", std::string(4097, 'x')}}}); }));
}

int main() { return ss_test::runAll(); }
