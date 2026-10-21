#include "doctest/doctest.h"
#include <chrono>
#include <filesystem>
#include "fixtures.hpp"
#include "ss/persist/migration.hpp"
#include "ss/persist/settings.hpp"
#include "ss/persist/store.hpp"
#include "ss/security/secret_store.hpp"

using namespace ss;
using namespace ss::persist;
using nlohmann::json;

// ---- migration ---------------------------------------------------------------------------------
namespace {
// Fake legacy "schema 0" format: the scene list was called "visuals". Migration 0 -> 1 renames it.
MigrationRegistry legacyRegistry(int* calls = nullptr) {
    MigrationRegistry r;
    MigrationStep s;
    s.fromMajor = 0; s.toMajor = 1; s.description = "0->1 rename visuals to scenes";
    s.apply = [calls](const std::string& in) -> Result<std::string> {
        if (calls) ++*calls;
        json j = json::parse(in, nullptr, false);
        if (j.is_discarded()) return makeError(ErrorCategory::DecodeFailure, "bad");
        if (j.contains("visuals")) { j["scenes"] = j["visuals"]; j.erase("visuals"); }
        j["schema"] = {{"major", 1}, {"minor", 0}};
        return j.dump();
    };
    r.add(std::move(s));
    return r;
}
std::string legacyFileBytes(const std::string& name) {
    json j = fx::toJ(fx::sampleDoc(name));
    j["visuals"] = j["scenes"]; j.erase("scenes");
    j["schema"]["major"] = 0;
    return makeEnvelope(j.dump(), "save", 1);
}
}  // namespace

TEST_CASE("REQ-X01: an older file is migrated in memory after its original is backed up; the source is never modified") {
    MemFileSystem fs; fs.createDirs("/s");
    FakeClock clock;
    const std::string original = legacyFileBytes("legacy");
    fs.put("/s/a.show", original);
    ShowStore store(fs, clock, "/s/a.show");
    int calls = 0;
    const auto reg = legacyRegistry(&calls);
    auto r = store.load(&reg);
    REQUIRE(r.ok());
    CHECK(calls == 1);
    CHECK(r.value().migrated);
    CHECK(r.value().parsed.doc.name == "legacy");
    CHECK(r.value().parsed.doc.scenes.size() == 2);
    CHECK(r.value().parsed.extras.migrationHistory == std::vector<std::string>{"0->1"});
    CHECK(r.value().migrationBackupPath == "/s/a.show.premigrate-v0");
    CHECK(fs.get(r.value().migrationBackupPath) == original);   // byte-identical backup
    CHECK(fs.get("/s/a.show") == original);                      // not touched by loading
    REQUIRE_FALSE(r.value().notes.empty());
    // saving writes the new format, keeps the old one as backup 1, and records the history
    JsonWriteOptions opt; opt.extras = r.value().parsed.extras;
    REQUIRE(store.save(r.value().parsed.doc, opt).ok());
    CHECK(fs.get(store.backupPath(1)) == original);
    auto again = store.load();                                   // no registry needed any more
    REQUIRE(again.ok());
    CHECK_FALSE(again.value().migrated);
    CHECK(again.value().parsed.extras.migrationHistory == std::vector<std::string>{"0->1"});
}

TEST_CASE("REQ-X01: without a migration path an older file is reported, not guessed at") {
    MemFileSystem fs; fs.createDirs("/s");
    FakeClock clock;
    fs.put("/s/a.show", legacyFileBytes("legacy"));
    ShowStore store(fs, clock, "/s/a.show");
    const auto r = store.load();                 // no registry
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().category == ErrorCategory::SchemaUnsupported);
    CHECK_FALSE(fs.exists("/s/a.show.premigrate-v0"));   // nothing was backed up or changed
}

TEST_CASE("REQ-X01: migration aborts before any step runs when the backup cannot be written") {
    MemFileSystem fs; fs.createDirs("/s");
    FakeClock clock;
    const std::string original = legacyFileBytes("legacy");
    fs.put("/s/a.show", original);
    ShowStore store(fs, clock, "/s/a.show");
    int calls = 0;
    const auto reg = legacyRegistry(&calls);
    fs.resetOpCount();
    MemFileSystem::Fault f; f.atOp = 1; f.kind = MemFileSystem::FaultKind::DiskFull;
    fs.setFault(f);
    const auto r = store.load(&reg);
    CHECK_FALSE(r.ok());
    CHECK(calls == 0);
    CHECK(fs.get("/s/a.show") == original);
}

TEST_CASE("REQ-X01: a failing migration step leaves the source and the backup intact") {
    MemFileSystem fs; fs.createDirs("/s");
    FakeClock clock;
    const std::string original = legacyFileBytes("legacy");
    fs.put("/s/a.show", original);
    ShowStore store(fs, clock, "/s/a.show");
    MigrationRegistry reg;
    MigrationStep s; s.fromMajor = 0; s.toMajor = 1; s.description = "bad";
    s.apply = [](const std::string&) -> Result<std::string> { return makeError(ErrorCategory::InternalFault, "boom"); };
    reg.add(s);
    CHECK_FALSE(store.load(&reg).ok());
    CHECK(fs.get("/s/a.show") == original);
    CHECK(fs.get("/s/a.show.premigrate-v0") == original);
    // a step that produces the wrong schema is rejected too
    MigrationRegistry wrong;
    MigrationStep w = s; w.apply = [](const std::string& in) -> Result<std::string> { return in; };
    fs.remove("/s/a.show.premigrate-v0");
    wrong.add(w);
    CHECK_FALSE(store.load(&wrong).ok());
}

TEST_CASE("REQ-X01: a newer-format file is never migrated, backed up over, or overwritten") {
    MemFileSystem fs; fs.createDirs("/s");
    MigrationRegistry reg;
    CHECK_FALSE(planMigration("/s/a.show", SchemaVersion{2, 0}, reg, fs).ok());
    const auto r = planMigration("/s/a.show", SchemaVersion{9, 1}, reg, fs);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().category == ErrorCategory::SchemaUnsupported);
    // older file without a registered path
    CHECK_FALSE(planMigration("/s/a.show", SchemaVersion{1, 0}, reg, fs, 3).ok());
    // current file: nothing to do
    auto cur = planMigration("/s/a.show", SchemaVersion{1, 0}, reg, fs);
    REQUIRE(cur.ok());
    CHECK_FALSE(cur.value().needed);
    CHECK(cur.value().backupPath.empty());
}

TEST_CASE("REQ-X01: multi-step migration chains and pre-migration backups never overwrite an earlier backup") {
    MemFileSystem fs; fs.createDirs("/s");
    MigrationRegistry reg;
    for (std::uint32_t from : {1u, 2u}) {
        MigrationStep s; s.fromMajor = from; s.toMajor = from + 1; s.description = std::to_string(from) + "->" + std::to_string(from + 1);
        s.apply = [](const std::string& in) -> Result<std::string> { return in; };
        reg.add(s);
    }
    fs.put("/s/a.show.premigrate-v1", "earlier");
    auto plan = planMigration("/s/a.show", SchemaVersion{1, 0}, reg, fs, 3);
    REQUIRE(plan.ok());
    CHECK(plan.value().needed);
    CHECK(plan.value().stepDescriptions == std::vector<std::string>{"1->2", "2->3"});
    CHECK(plan.value().backupPath == "/s/a.show.premigrate-v1-2");
    CHECK(reg.from(7) == nullptr);
}

// ---- local settings ----------------------------------------------------------------------------
TEST_CASE("REQ-B03: local settings hold device hints and secret references, never secret values") {
    MemFileSystem fs; fs.createDirs("/cfg");
    LocalSettings s;
    s.recordingFolder = "/home/user/Videos/StageSignal";
    s.deviceHints.push_back({"audioInput", "{0.0.1.00000000}.{abc-123}", "Focusrite"});
    s.deviceHints.push_back({"display", "DISPLAY\\SAM0F1A\\4&1d", "Wall"});
    security::InMemorySecretStore store;
    const auto ref = security::makeSecretRef("YouTube main");
    REQUIRE(store.put(ref, "super-secret-stream-key-123").ok());
    s.secretRefs.push_back({"YouTube main", ref});

    REQUIRE(saveSettings(fs, "/cfg/settings.json", s).ok());
    const std::string onDisk = fs.get("/cfg/settings.json");
    CHECK(onDisk.find("super-secret-stream-key-123") == std::string::npos);
    CHECK(onDisk.find("secret:youtube-main") != std::string::npos);
    auto back = loadSettings(fs, "/cfg/settings.json");
    REQUIRE(back.ok());
    CHECK(back.value().deviceHints.size() == 2);
    CHECK(back.value().secretRefs[0].ref == ref);
    CHECK(back.value().recordingFolder == s.recordingFolder);
    CHECK(store.exists(back.value().secretRefs[0].ref));
}

TEST_CASE("REQ-B03: settings reject plain secrets, bad references and URLs") {
    LocalSettings s;
    s.secretRefs.push_back({"x", "hunter2"});
    CHECK_FALSE(settingsToJson(s).ok());
    s.secretRefs.clear();
    s.recordingFolder = "https://evil.example/upload";
    CHECK_FALSE(settingsToJson(s).ok());
    s.recordingFolder.clear();
    s.deviceHints.push_back({"keylogger", "id", "x"});
    CHECK_FALSE(settingsToJson(s).ok());

    LocalSettings good;
    good.secretRefs.push_back({"k", "secret:k"});
    auto text = settingsToJson(good);
    REQUIRE(text.ok());
    auto j = json::parse(text.value());
    j["secretRefs"][0]["value"] = "plain-secret";
    const auto r = settingsFromJson(j.dump());
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().detail.find("plain-secret") == std::string::npos);
    j = json::parse(text.value()); j["schema"] = 9;
    CHECK(settingsFromJson(j.dump()).error().category == ErrorCategory::SchemaUnsupported);
    CHECK_FALSE(settingsFromJson("{").ok());
    CHECK_FALSE(settingsFromJson(std::string(2u << 20, ' ')).ok());
}

TEST_CASE("REQ-X01: missing settings file yields defaults; settings are saved atomically") {
    MemFileSystem fs; fs.createDirs("/cfg");
    auto r = loadSettings(fs, "/cfg/none.json");
    REQUIRE(r.ok());
    CHECK(r.value().deviceHints.empty());
    LocalSettings s; s.lastShowPath = "/shows/a.show";
    REQUIRE(saveSettings(fs, "/cfg/s.json", s).ok());
    fs.resetOpCount();
    MemFileSystem::Fault f; f.atOp = 2; f.kind = MemFileSystem::FaultKind::Crash;   // crash after the temp write, before rename
    fs.setFault(f);
    LocalSettings s2; s2.lastShowPath = "/shows/b.show";
    CHECK_FALSE(saveSettings(fs, "/cfg/s.json", s2).ok());
    fs.reboot();
    CHECK(loadSettings(fs, "/cfg/s.json").value().lastShowPath == "/shows/a.show");
}

// ---- filesystem implementations ---------------------------------------------------------------
TEST_CASE("REQ-X01: StdFileSystem behaves like the in-memory fake for the operations persist uses") {
    namespace fsys = std::filesystem;
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fsys::path root = fsys::temp_directory_path() / ("ss_persist_test_" + stamp);
    const std::string dir = root.generic_string();
    StdFileSystem fs;
    REQUIRE(fs.createDirs(dir).ok());
    FakeClock clock;
    ShowStore store(fs, clock, dir + "/real.show");
    REQUIRE(store.save(fx::sampleDoc("one")).ok());
    clock.advance(secToNs(2));
    REQUIRE(store.save(fx::sampleDoc("two")).ok());
    REQUIRE(store.save(fx::sampleDoc("three")).ok());
    auto r = store.load();
    REQUIRE(r.ok());
    CHECK(r.value().parsed.doc.name == "three");
    CHECK(store.loadFile(store.backupPath(1)).value().parsed.doc.name == "two");
    CHECK(store.loadFile(store.backupPath(2)).value().parsed.doc.name == "one");
    auto names = fs.listDir(dir);
    REQUIRE(names.ok());
    CHECK(names.value().size() == 3);   // real.show, .bak1, .bak2 and no temp files
    CHECK_FALSE(fs.exists(dir + "/real.show.tmp"));
    CHECK_FALSE(fs.readFile(dir + "/real.show", 10).ok());                 // oversize refused before read
    CHECK(fs.readFile(dir + "/real.show", 10).error().category == ErrorCategory::ResourceExhausted);
    CHECK_FALSE(fs.readFile(dir + "/missing", 100).ok());
    CHECK(fs.remove(dir + "/missing").ok());                                // idempotent
    std::error_code ec;
    fsys::remove_all(root, ec);
}

TEST_CASE("REQ-X01: path helpers") {
    CHECK(parentDir("/a/b/c.txt") == "/a/b");
    CHECK(parentDir("c.txt") == "");
    CHECK(baseName("/a/b/c.txt") == "c.txt");
    CHECK(joinPath("/a", "b") == "/a/b");
    CHECK(joinPath("/a/", "b") == "/a/b");
    CHECK(joinPath("", "b") == "b");
}

TEST_CASE("REQ-X01: MemFileSystem fault injection semantics") {
    MemFileSystem fs; fs.createDirs("/d");
    REQUIRE(fs.writeFile("/d/a", "0123456789", true).ok());
    fs.resetOpCount();
    MemFileSystem::Fault f; f.atOp = 1; f.kind = MemFileSystem::FaultKind::Crash; f.tornFraction = 0.5;
    fs.setFault(f);
    CHECK_FALSE(fs.writeFile("/d/a", "ABCDEFGHIJ", true).ok());
    CHECK(fs.crashed());
    CHECK_FALSE(fs.readFile("/d/a", 100).ok());      // everything fails until reboot
    fs.reboot();
    CHECK(fs.get("/d/a") == "ABCDE");                // torn write
    CHECK_FALSE(fs.writeFile("/nodir/x", "a", true).ok());
}
