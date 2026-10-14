// MOD-07 asset manifest + own SHA-256 (REQ-S15, T-LICENSE).
#include "media_test_util.h"
#include <algorithm>
#include "media/asset_manifest.h"
#include "media/sha256.h"
#include "media/placeholder.h"

using namespace ss; using namespace ss::media;

static std::string hexOf(const std::string& s) { return sha256Hex(s.data(), s.size()); }

TEST(sha256_nist_vectors) {
  CHECK(hexOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hexOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(hexOf("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
  Sha256 s; std::string chunk(1000, 'a');                                  // NIST long message: one million 'a'
  for (int i = 0; i < 1000; ++i) s.update(chunk.data(), chunk.size());
  CHECK(s.finalHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(sha256_padding_boundaries_and_streaming) {
  struct V { size_t n; const char* h; } v[] = {
    {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"}, {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
    {57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"}, {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
    {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"}, {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
    {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"}, {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
    {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"}};
  for (auto& t : v) {
    std::string m(t.n, 'a'); CHECK(hexOf(m) == t.h);
    Sha256 s; for (size_t i = 0; i < m.size(); i += 7) s.update(m.data() + i, std::min<size_t>(7, m.size() - i));   // odd-sized pieces
    CHECK(s.finalHex() == t.h);
  }
  Sha256 s; s.update("abc", 3); (void)s.finalHex(); s.reset(); s.update("abc", 3);                                  // reuse after reset
  CHECK(s.finalHex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(isHex64(hexOf("x")) && !isHex64("ABC") && !isHex64(std::string(64, 'G')) && !isHex64(std::string(63, 'a')));
  std::string dir = mt::tmpDir("sha"), path = dir + "/f.bin"; std::string big(200000, 'q'); mt::writeFile(path, std::vector<uint8_t>(big.begin(), big.end()));
  std::string hex; uint64_t sz = 0; CHECK(sha256FileHex(path, hex, &sz).ok() && hex == hexOf(big) && sz == 200000);
  CHECK(!sha256FileHex(dir + "/absent", hex).ok());
}

TEST(spdx_expression_syntax) {
  for (const char* ok : {"CC0-1.0", "MIT", "MIT OR Unlicense", "Apache-2.0 AND MIT", "(MIT OR Apache-2.0) AND BSD-3-Clause", "GPL-2.0-or-later WITH Classpath-exception-2.0",
                         "LicenseRef-Custom-1", "LGPL-2.1+", "CC-BY-4.0"})
    CHECK(isValidSpdxExpression(ok));
  for (const char* bad : {"", "MIT OR", "OR MIT", "(MIT", "MIT)", "MIT AND AND Apache-2.0", "NONE", "NOASSERTION", "MIT WITH", "MIT Apache-2.0", "free to use!", "CC0 1.0 universal"})
    CHECK(!isValidSpdxExpression(bad));
}

// ---- fixture assets directory ---------------------------------------------------------------------------------------
static AssetEntry entryFor(const std::string& dir, const std::string& id, const std::string& file, const std::vector<uint8_t>& bytes, int w, int h) {
  mt::writeFile(dir + "/" + file, bytes);
  AssetEntry e; e.id = id; e.version = "1.0.0"; e.type = "still"; e.path = file; e.creator = "Test Creator"; e.retrievalDate = "2026-10-14";
  e.license = "CC0-1.0"; e.copyright = "(c) test"; e.attributionText = "none"; e.attributionLocation = "n/a"; e.commercialUse = 1; e.modification = 1;
  e.provenanceKind = "original"; e.provenanceNotes = "generated"; e.sha256Original = e.sha256Distributed = sha256Hex(bytes.data(), bytes.size());
  e.width = w; e.height = h; e.sizeBytes = bytes.size(); e.tiers = {"BASE", "STANDARD"};
  return e;
}
struct Fixture {
  std::string dir; Manifest m;
  Fixture() {
    dir = mt::tmpDir("manifest"); m.generator = "test";
    m.assets.push_back(entryFor(dir, "t.a", "a.png", mt::png(16, 8), 16, 8));
    m.assets.push_back(entryFor(dir, "t.b", "sub/b.png", (std::filesystem::create_directories(dir + "/sub"), mt::png(10, 10)), 10, 10));
    write();
  }
  void write() { std::string t = serializeManifest(m); mt::writeFile(dir + "/manifest.json", std::vector<uint8_t>(t.begin(), t.end())); }
  ManifestReport run(VerifyOptions o = VerifyOptions()) { write(); return verifyManifest(dir, o); }
};
static bool has(const ManifestReport& r, const char* code) { for (auto& i : r.issues) if (i.code == code) return true; return false; }
static bool hasFor(const ManifestReport& r, const char* code, const char* id) { for (auto& i : r.issues) if (i.code == code && i.assetId == id) return true; return false; }

TEST(manifest_valid_set_passes) {
  Fixture f; ManifestReport r = f.run();
  CHECK(r.ok && r.issues.empty() && r.entriesChecked == 2);
  if (!r.ok) std::printf("%s", r.summary().c_str());
  Manifest back; CHECK(parseManifest(serializeManifest(f.m), back).ok() && back.assets.size() == 2 && back.assets[0].sha256Distributed == f.m.assets[0].sha256Distributed);
}

TEST(manifest_detects_checksum_mismatch_missing_and_unlisted_files) {
  { Fixture f; auto b = mt::png(16, 8, mt::solidRed); mt::writeFile(f.dir + "/a.png", b);                  // different bytes, same name
    ManifestReport r = f.run(); CHECK(!r.ok && hasFor(r, "checksum-mismatch", "t.a") && !hasFor(r, "checksum-mismatch", "t.b")); }
  { Fixture f; std::vector<uint8_t> b = mt::png(10, 10); b[b.size() / 2] ^= 1; mt::writeFile(f.dir + "/sub/b.png", b);   // single flipped bit
    ManifestReport r = f.run(); CHECK(!r.ok && hasFor(r, "checksum-mismatch", "t.b")); }
  { Fixture f; std::filesystem::remove(f.dir + "/a.png"); ManifestReport r = f.run(); CHECK(!r.ok && hasFor(r, "file-missing", "t.a")); }
  { Fixture f; mt::writeFile(f.dir + "/stray.png", mt::png(4, 4)); ManifestReport r = f.run(); CHECK(!r.ok && has(r, "unlisted-file"));
    VerifyOptions o; o.failOnUnlistedFiles = false; CHECK(f.run(o).ok); }
  { Fixture f; f.m.assets[0].sizeBytes += 1; ManifestReport r = f.run(); CHECK(!r.ok && hasFor(r, "size-mismatch", "t.a")); }
  { Fixture f; f.m.assets[0].width = 17; ManifestReport r = f.run(); CHECK(!r.ok && hasFor(r, "dimension-mismatch", "t.a")); }
  { Fixture f; f.m.assets[0].sha256Distributed[0] = f.m.assets[0].sha256Distributed[0] == '0' ? '1' : '0';
    ManifestReport r = f.run(); CHECK(!r.ok && hasFor(r, "checksum-mismatch", "t.a") && hasFor(r, "provenance-mismatch", "t.a")); }
  { std::string d = mt::tmpDir("nomanifest"); ManifestReport r = verifyManifest(d); CHECK(!r.ok && has(r, "manifest-missing")); }
  { Fixture f; mt::writeFile(f.dir + "/manifest.json", std::vector<uint8_t>{'{', 'x'}); ManifestReport r = verifyManifest(f.dir); CHECK(!r.ok && has(r, "manifest-parse")); }
  { Fixture f; std::string t = "{\"schemaVersion\": 99, \"assets\": []}"; mt::writeFile(f.dir + "/manifest.json", std::vector<uint8_t>(t.begin(), t.end()));
    ManifestReport r = verifyManifest(f.dir); CHECK(!r.ok && has(r, "manifest-parse")); }
}

TEST(manifest_requires_all_spec_fields) {
  struct Case { const char* what; void (*mut)(AssetEntry&); const char* code; } cases[] = {
    {"creator", [](AssetEntry& e) { e.creator.clear(); }, "missing-field"},
    {"license", [](AssetEntry& e) { e.license.clear(); }, "missing-field"},
    {"version", [](AssetEntry& e) { e.version.clear(); }, "missing-field"},
    {"copyright", [](AssetEntry& e) { e.copyright.clear(); }, "missing-field"},
    {"attribution", [](AssetEntry& e) { e.attributionText.clear(); }, "missing-field"},
    {"commercial", [](AssetEntry& e) { e.commercialUse = -1; }, "missing-field"},
    {"modification", [](AssetEntry& e) { e.modification = -1; }, "missing-field"},
    {"retrieval", [](AssetEntry& e) { e.retrievalDate.clear(); }, "missing-field"},
    {"sha original", [](AssetEntry& e) { e.sha256Original = "abc"; }, "missing-field"},
    {"flash", [](AssetEntry& e) { e.flash.maxFlashHz = -1; }, "missing-field"},
    {"tiers", [](AssetEntry& e) { e.tiers.clear(); }, "missing-field"},
    {"provenance kind", [](AssetEntry& e) { e.provenanceKind.clear(); }, "missing-field"},
    {"derivative w/o source", [](AssetEntry& e) { e.provenanceKind = "derivative"; e.originalUrl = "https://x.example/a"; }, "missing-field"},
    {"third-party w/o url", [](AssetEntry& e) { e.provenanceKind = "third-party"; }, "missing-field"},
    {"bad spdx", [](AssetEntry& e) { e.license = "free to use"; }, "bad-license"},
    {"NOASSERTION", [](AssetEntry& e) { e.license = "NOASSERTION"; }, "bad-license"},
    {"bad date", [](AssetEntry& e) { e.retrievalDate = "14/10/2026"; }, "bad-date"},
    {"bad tier", [](AssetEntry& e) { e.tiers = {"GOLD"}; }, "bad-tier"},
    {"bad type", [](AssetEntry& e) { e.type = "svg"; }, "bad-type"},
    {"traversal", [](AssetEntry& e) { e.path = "../a.png"; }, "bad-path"},
    {"absolute", [](AssetEntry& e) { e.path = "/etc/passwd"; }, "bad-path"},
    {"drive", [](AssetEntry& e) { e.path = "C:/x.png"; }, "bad-path"},
    {"url path", [](AssetEntry& e) { e.path = "http://x/a.png"; }, "bad-path"},
    {"backslash", [](AssetEntry& e) { e.path = "a\\b.png"; }, "bad-path"},
    {"unknown prov", [](AssetEntry& e) { e.provenanceKind = "borrowed"; }, "bad-provenance"},
  };
  for (auto& c : cases) {
    Fixture f; c.mut(f.m.assets[0]); ManifestReport r = f.run();
    CHECK(!r.ok && hasFor(r, c.code, "t.a"));
    if (r.ok || !hasFor(r, c.code, "t.a")) std::printf("  case: %s\n%s", c.what, r.summary().c_str());
  }
  { Fixture f; f.m.assets[1].id = "t.a"; ManifestReport r = f.run(); CHECK(!r.ok && has(r, "duplicate-id")); }
  { Fixture f; f.m.assets[1].path = "a.png"; ManifestReport r = f.run(); CHECK(!r.ok && has(r, "duplicate-path")); }
  { Fixture f; f.m.assets[0].provenanceKind = "derivative"; f.m.assets[0].derivedFrom = {"t.b"}; f.m.assets[0].originalUrl = "urn:x"; f.m.assets[0].sha256Original = std::string(64, 'a');
    ManifestReport r = f.run(); CHECK(r.ok); }                                                             // legitimate derivative passes
  { Fixture f; f.m.assets[1].tiers = {"SHOW"}; VerifyOptions o; o.requireTier = "BASE"; std::filesystem::remove(f.dir + "/sub/b.png");
    ManifestReport r = f.run(o); CHECK(!has(r, "file-missing")); }                                         // not shipped on BASE -> not required
}

TEST(manifest_checks_gif_duration_and_garbage_images) {
  Fixture f;
  ss_gif::Spec s; s.w = 4; s.h = 4; s.palette = mt::pal4(); s.loop = 0; s.frames = {mt::gframe(0,0,4,4,0,10,1), mt::gframe(0,0,4,4,1,10,1)};
  AssetEntry g = entryFor(f.dir, "t.g", "g.gif", ss_gif::build(s), 4, 4); g.type = "animation"; g.durationMs = 200; f.m.assets.push_back(g);
  CHECK(f.run().ok);
  f.m.assets.back().durationMs = 300; ManifestReport r = f.run(); CHECK(!r.ok && hasFor(r, "duration-mismatch", "t.g"));
  f.m.assets.back().durationMs = 200;
  std::vector<uint8_t> junk(100, 0x41); AssetEntry j = entryFor(f.dir, "t.j", "junk.png", junk, 4, 4); f.m.assets.push_back(j);
  r = f.run(); CHECK(!r.ok && hasFor(r, "unreadable-image", "t.j"));                                      // hash matches but it is not an image
}

TEST(bundled_assets_manifest_verifies) {
  // Runs from the repository root (CTest WORKING_DIRECTORY). This is the packaging gate for the shipped assets.
  ManifestReport r = verifyManifest("assets");
  CHECK(r.ok); if (!r.ok) std::printf("%s", r.summary().c_str());
  CHECK(r.entriesChecked >= 3);
  Manifest m; std::vector<uint8_t> b; CHECK(readFileBounded("assets/manifest.json", 1 << 20, b).ok());
  CHECK(parseManifest(std::string(b.begin(), b.end()), m).ok());
  bool card = false, ph = false, gif = false;
  for (auto& e : m.assets) {
    CHECK(e.license == "CC0-1.0" && e.creator == "Circuit Drift Labs" && e.provenanceKind == "original" && e.sha256Original == e.sha256Distributed && isHex64(e.sha256Distributed));
    if (e.id == "cdl.testcard.320x240") card = true;
    if (e.id == "cdl.placeholder.missing-media.320x240") ph = true;
    if (e.id == "cdl.loop.dot.64x48") gif = true;
  }
  CHECK(card && ph && gif);
  CHECK(m.decoders.empty());                                                                               // honest: no decoder bundled/enabled
  // the shipped placeholder image is exactly the runtime placeholder
  DecodedStill d; CHECK(validateAndDecodeStillFile("assets/missing_media_320x240.png", limitsFor(Tier::Base), d).ok());
  Frame ref = makePlaceholderFrame(320, 240), got = frameFromArgb(d.argb.data(), d.w, d.h, 0);
  CHECK(got.w == ref.w && got.px == ref.px);
  DecodedGif g; CHECK(validateAndDecodeGifFile("assets/loop_dot_64x48.gif", limitsFor(Tier::Base), g).ok() && g.frameCount == 12 && g.loopCount == 0 && g.totalMs() == 960);
  // tampering with a copy of the shipped set is caught
  std::string copy = mt::tmpDir("tamper"); for (auto& e : std::filesystem::directory_iterator("assets")) std::filesystem::copy_file(e.path(), copy + "/" + e.path().filename().string());
  CHECK(verifyManifest(copy).ok);
  std::vector<uint8_t> t; readFileBounded(copy + "/testcard_320x240.png", 1 << 20, t); t[t.size() - 20] ^= 0x10; mt::writeFile(copy + "/testcard_320x240.png", t);
  ManifestReport rt = verifyManifest(copy); CHECK(!rt.ok && hasFor(rt, "checksum-mismatch", "cdl.testcard.320x240"));
}

int main() { return ss_test::runAll(); }
