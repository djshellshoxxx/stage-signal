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
// A fake "schema 0 -> 1" migration for the framework tests: v0 files call the scenes list "visuals".
MigrationRegistry fakeRegistry() {
    MigrationRegistry r;
    MigrationStep s;
    s.fromMajor = 0; s.toMajor = 1; s.description = "0->1 rename visuals to scenes";
    s.apply = [](const std::string& in) -> Result<std::string> {
        json j = json::parse(in, nullptr, false);
        if (j.is_discarded()) return makeError(ErrorCategory::DecodeFailure, "bad");
        if (j.contains("visuals")) { j["scenes"] = j["visuals"]; j.erase("visuals"); }
        j["schema"] = {{"major", 1}, {"minor", 0}};
        return j.dump();
    };
    r.add(std::move(s));
    return r;
}
// migration for a hypothetical future: 1 -> 2 (used to prove "newer is never touched")
std::string oldFormatFile() {
    json j = fx::toJ(fx::sampleDoc("legacy"));
    j["visuals"] = j["scenes"]; j.erase("scenes");
    j["schema"]["major"] = 0;
    return j.dump();
}
}  // namespace

TEST_CASE("REQ-X01: migration copies the original to a backup before anything else") {
    // schema "0" cannot be produced by the current writer, so exercise the framework with a registry that starts at 0
    // by planning against target 1 and a file whose schema.major is 0 is rejected by peekSchema; use major 1 -> target 2.
    MemFileSystem fs; fs.createDirs("/s");
    MigrationRegistry reg;
    MigrationStep s;
    s.fromMajor = 1; s.toMajor = 2; s.description = "1->2";
    s.apply = [](const std::string& in) -> Result<std::string> { json j = json::parse(in); j["schema"]["major"] = 2; return j.dump(); };
    reg.add(s);
    const std::string original = makeEnvelope(showToJson(fx::sampleDoc("old")).value(), "save", 1);
    fs.put("/s/a.show", original);
    auto plan = planMigration("/s/a.show", SchemaVersion{1, 0}, reg, fs, 2);
    REQUIRE(plan.ok());
    CHECK(plan.value().needed);
    CHECK(plan.value().stepDescriptions == std::vector<std::string>{"1->2"});
    CHECK(plan.value().backupPath == "/s/a.show.premigrate-v1");

    // backup failure aborts before any migration step runs
    int calls = 0;
    MigrationRegistry counting;
    MigrationStep c = s; c.apply = [&](const std::string& in) -> Result<std::string> { ++calls; return s.apply(in); };
    counting.add(c);
    MemFileSystem::Fault f; f.atOp = 1; f.kind = MemFileSystem::FaultKind::DiskFull;
    fs.resetOpCount(); fs.setFault(f);
    const auto body = openEnvelope(original).value();
    CHECK_FALSE(applyMigration(fs, plan.value(), original, body, counting).ok());
    CHECK(calls == 0);
    fs.clearFault();
    fs.remove(plan.value().backupPath);

    // the migrated document only validates once the schema major matches this build; here the target (2) is not
    // supported by showFromJson, so applyMigration reports SchemaUnsupported AFTER having made the backup.
    const auto res = applyMigration(fs, plan.value(), original, body, counting);
    CHECK(calls == 1);
    CHECK_FALSE(res.ok());
    CHECK(fs.get(plan.value().backupPath) == original);   // backup is byte-identical and was made first
    CHECK(fs.get("/s/a.show") == original);                // source never modified
}

TEST_CASE("REQ-X01: migration framework upgrades an older file into the current schema in memory") {
    MemFileSystem fs; fs.createDirs("/s");
    FakeClock clock;
    // peekSchema rejects major 0, so register the legacy format as major 1 -> 1 style by hand: use a custom flow.
    const auto reg = fakeRegistry();
    const std::string text = oldFormatFile();
    // direct step test: the registered step produces a loadable current-schema document
    const MigrationStep* step = reg.from(0);
    REQUIRE(step != nullptr);
    auto out = step->apply(text);
    REQUIRE(out.ok());
    auto parsed = showFromJson(out.value());
    REQUIRE(parsed.ok());
    CHECK(parsed.value().doc.scenes.size() == 2);
    CHECK(reg.from(7) == nullptr);
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
}

TEST_CASE("REQ-X01: pre-migration backups never overwrite an earlier backup") {
    MemFileSystem fs; fs.createDirs("/s");
    MigrationRegistry reg;
    MigrationStep s; s.fromMajor = 1; s.toMajor = 2; s.apply = [](const std::string& in) -> Result<std::string> { return in; };
    reg.add(s);
    fs.put("/s/a.show.premigrate-v1", "earlier");
    auto plan = planMigration("/s/a.show", SchemaVersion{1, 0}, reg, fs, 2);
    REQUIRE(plan.ok());
    CHECK(plan.value().backupPath == "/s/a.show.premigrate-v1-2");
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
