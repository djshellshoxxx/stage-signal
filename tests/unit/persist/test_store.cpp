#include "doctest/doctest.h"
#include <string>
#include "fixtures.hpp"
#include "ss/persist/store.hpp"

using namespace ss;
using namespace ss::persist;

static const char* kPath = "/shows/main.show";

namespace {
struct Env {
    MemFileSystem fs;
    FakeClock clock;
    ShowStore store{fs, clock, kPath};
    Env() { fs.createDirs("/shows"); }
    Status saveNamed(const std::string& name) { clock.advance(secToNs(1)); return store.save(fx::sampleDoc(name)); }
    std::string loadedName() { auto r = store.load(); return r.ok() ? r.value().parsed.doc.name : std::string("<load failed>"); }
};
}  // namespace

TEST_CASE("REQ-X01: save then load round-trips and leaves no temp file") {
    Env e;
    REQUIRE(e.saveNamed("one").ok());
    auto r = e.store.load();
    REQUIRE(r.ok());
    CHECK(r.value().parsed.doc.name == "one");
    CHECK(r.value().source == LoadSource::Primary);
    CHECK(r.value().info.hasEnvelope);
    CHECK_FALSE(e.fs.exists(std::string(kPath) + ".tmp"));
    CHECK_FALSE(e.fs.exists(e.store.backupPath(1)));   // nothing to back up on first save
}

TEST_CASE("REQ-X01: the three previous valid saves are retained") {
    Env e;
    for (const char* n : {"v1", "v2", "v3", "v4", "v5"}) REQUIRE(e.saveNamed(n).ok());
    CHECK(e.loadedName() == "v5");
    auto b = [&](int n) { return e.store.loadFile(e.store.backupPath(n)).value().parsed.doc.name; };
    CHECK(b(1) == "v4");
    CHECK(b(2) == "v3");
    CHECK(b(3) == "v2");
    CHECK_FALSE(e.fs.exists(e.store.backupPath(4)));
}

TEST_CASE("REQ-X01: a damaged previous file is not rotated into the backups") {
    Env e;
    REQUIRE(e.saveNamed("v1").ok());
    REQUIRE(e.saveNamed("v2").ok());      // bak1 = v1
    e.fs.put(kPath, "STAGESIGNAL-SAVE 1 kind=save saved=1 bytes=5 sha256=00\nhello");   // corrupt primary
    REQUIRE(e.saveNamed("v3").ok());
    CHECK(e.store.loadFile(e.store.backupPath(1)).value().parsed.doc.name == "v1");   // unchanged: garbage did not displace it
    CHECK(e.loadedName() == "v3");
}

TEST_CASE("REQ-X01: crash at every write step leaves a valid old or new version (crash-at-write)") {
    // Count the mutating operations of a save that rotates backups.
    auto prepare = [](Env& e) { for (const char* n : {"a1", "a2", "a3", "a4"}) REQUIRE(e.saveNamed(n).ok()); e.fs.resetOpCount(); };
    std::size_t total = 0;
    {
        Env e; prepare(e);
        REQUIRE(e.saveNamed("b").ok());
        total = e.fs.opCount();
        CHECK(total >= 7);
    }
    for (const auto kind : {MemFileSystem::FaultKind::Crash}) {
        for (double torn : {0.0, 0.4, 0.99}) {
            for (std::size_t k = 1; k <= total; ++k) {
                Env e; prepare(e);
                MemFileSystem::Fault f; f.atOp = k; f.kind = kind; f.tornFraction = torn;
                e.fs.setFault(f);
                const auto st = e.saveNamed("b");
                CAPTURE(k); CAPTURE(torn);
                e.fs.reboot();
                const std::string name = e.loadedName();
                CHECK((name == "a4" || name == "b"));
                if (st.ok()) CHECK(name == "b");   // only the trailing best-effort cleanup can fail after the commit point
                // the system keeps working: the next save succeeds and wins
                REQUIRE(e.saveNamed("c").ok());
                CHECK(e.loadedName() == "c");
            }
        }
    }
}

TEST_CASE("REQ-X01: crash during the very first save never produces a half-valid show") {
    Env probe;
    REQUIRE(probe.saveNamed("first").ok());
    const std::size_t total = probe.fs.opCount();
    for (std::size_t k = 1; k <= total; ++k) {
        Env e;
        MemFileSystem::Fault f; f.atOp = k;
        e.fs.setFault(f);
        e.saveNamed("first");
        e.fs.reboot();
        auto r = e.store.load();
        CAPTURE(k);
        if (r.ok()) CHECK(r.value().parsed.doc.name == "first");
    }
}

TEST_CASE("REQ-X01: flush before rename is mandatory (unflushed data is lost on crash)") {
    Env e;
    REQUIRE(e.saveNamed("good").ok());
    // Simulate a naive writer: write temp WITHOUT flush, rename over the primary, crash.
    const auto json = showToJson(fx::sampleDoc("naive")).value();
    REQUIRE(e.fs.writeFile("/shows/naive.tmp", makeEnvelope(json, "save", 1), false).ok());
    REQUIRE(e.fs.rename("/shows/naive.tmp", kPath).ok());
    MemFileSystem::Fault f; f.atOp = e.fs.opCount() + 1;
    e.fs.setFault(f);
    e.fs.writeFile("/shows/other", "x", true);   // any op triggers the crash
    e.fs.reboot();
    CHECK(e.fs.get(kPath).empty());               // proves the model catches missing flushes
    // ... while our store flushes, so its data survives the same fault
    Env ok;
    REQUIRE(ok.saveNamed("one").ok());
    REQUIRE(ok.saveNamed("two").ok());
    MemFileSystem::Fault g; g.atOp = ok.fs.opCount() + 1;
    ok.fs.setFault(g);
    ok.fs.writeFile("/shows/other", "x", true);
    ok.fs.reboot();
    CHECK(ok.loadedName() == "two");
}

TEST_CASE("REQ-X01: save failure (disk full) keeps the old file and reports the error") {
    Env probe;
    REQUIRE(probe.saveNamed("a").ok());
    probe.fs.resetOpCount();
    REQUIRE(probe.saveNamed("b").ok());
    const std::size_t total = probe.fs.opCount();   // second save rotates
    for (std::size_t k = 1; k <= total; ++k) {
        Env e;
        REQUIRE(e.saveNamed("a").ok());
        e.fs.resetOpCount();
        MemFileSystem::Fault f; f.atOp = k; f.kind = MemFileSystem::FaultKind::DiskFull;
        e.fs.setFault(f);
        const auto st = e.saveNamed("b");
        CAPTURE(k);
        e.fs.clearFault();
        if (!st.ok()) {
            CHECK(e.loadedName() == "a");                        // old file intact
            CHECK_FALSE(st.error().detail.empty());
            REQUIRE(e.saveNamed("b").ok());                      // retry works
        }
        CHECK(e.loadedName() == "b");
    }
}

TEST_CASE("REQ-X01: a damaged primary falls back to the newest valid backup and says so") {
    Env e;
    for (const char* n : {"v1", "v2", "v3"}) REQUIRE(e.saveNamed(n).ok());
    std::string bytes = e.fs.get(kPath);
    bytes[bytes.size() / 2] ^= 0x55;     // bit rot
    e.fs.put(kPath, bytes);
    auto r = e.store.load();
    REQUIRE(r.ok());
    CHECK(r.value().parsed.doc.name == "v2");
    CHECK(r.value().source == LoadSource::Backup1);
    REQUIRE_FALSE(r.value().notes.empty());
    // truncated primary
    e.fs.put(kPath, bytes.substr(0, bytes.size() / 2));
    CHECK(e.loadedName() == "v2");
    // everything damaged
    for (int n = 1; n <= 3; ++n) e.fs.put(e.store.backupPath(n), "junk");
    CHECK_FALSE(e.store.load().ok());
}

TEST_CASE("REQ-S14: invalid file never replaces anything; load returns an error and no state") {
    Env e;
    REQUIRE(e.saveNamed("keep").ok());
    auto j = fx::toJ(fx::sampleDoc("evil"));
    j["script"] = "x";
    e.fs.put("/shows/bad.show", makeEnvelope(j.dump(), "save", 1));
    ShowStore bad(e.fs, e.clock, "/shows/bad.show");
    CHECK_FALSE(bad.load().ok());
    CHECK(e.loadedName() == "keep");   // unrelated live file untouched
}

TEST_CASE("REQ-X01: oversize files are refused before they are read") {
    Env e;
    e.fs.put(kPath, std::string(model::Limits::kMaxJsonBytes + 4096, 'x'));
    const auto r = e.store.load();
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().category == ErrorCategory::ResourceExhausted);
}

TEST_CASE("REQ-X01: a file written by a newer format is never overwritten or silently skipped") {
    Env e;
    auto j = fx::toJ(fx::sampleDoc("future"));
    j["schema"]["major"] = 2;
    const std::string future = makeEnvelope(j.dump(), "save", 5);
    e.fs.put(kPath, future);
    e.fs.put(e.store.backupPath(1), makeEnvelope(showToJson(fx::sampleDoc("old")).value(), "save", 1));
    const auto st = e.store.save(fx::sampleDoc("mine"));
    REQUIRE_FALSE(st.ok());
    CHECK(st.error().category == ErrorCategory::SchemaUnsupported);
    CHECK(e.fs.get(kPath) == future);
    const auto r = e.store.load();            // does not fall back to the old backup behind the user's back
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().category == ErrorCategory::SchemaUnsupported);
    // same for a newer minor
    auto m = fx::toJ(fx::sampleDoc("future-minor"));
    m["schema"]["minor"] = 3;
    const std::string futureMinor = makeEnvelope(m.dump(), "save", 6);
    e.fs.put(kPath, futureMinor);
    CHECK_FALSE(e.store.save(fx::sampleDoc("mine")).ok());
    CHECK(e.fs.get(kPath) == futureMinor);
}

TEST_CASE("REQ-X01: invalid document is rejected before touching the filesystem") {
    Env e;
    REQUIRE(e.saveNamed("good").ok());
    e.fs.resetOpCount();
    auto bad = fx::sampleDoc("bad");
    bad.cues[0].sceneId = 777;
    CHECK_FALSE(e.store.save(bad).ok());
    CHECK(e.fs.opCount() == 0);
    CHECK(e.loadedName() == "good");
}

TEST_CASE("REQ-X01: stale temp files from a crash are cleaned up by the next save") {
    Env e;
    e.fs.put(std::string(kPath) + ".tmp", "partial");
    REQUIRE(e.saveNamed("x").ok());
    CHECK_FALSE(e.fs.exists(std::string(kPath) + ".tmp"));
}

TEST_CASE("REQ-X01: envelope detects length and checksum errors") {
    const std::string body = "{\"a\":1}";
    const auto env = makeEnvelope(body, "save", 42);
    EnvelopeInfo info;
    CHECK(openEnvelope(env, &info).value() == body);
    CHECK(info.savedWallUnixMs == 42);
    CHECK(info.hasEnvelope);
    CHECK_FALSE(openEnvelope(env.substr(0, env.size() - 1)).ok());
    std::string flipped = env; flipped.back() = 'X';
    CHECK_FALSE(openEnvelope(flipped).ok());
    CHECK_FALSE(openEnvelope("").ok());
    CHECK_FALSE(openEnvelope("STAGESIGNAL-SAVE 1 kind=save").ok());
    CHECK(openEnvelope(body).value() == body);   // raw JSON accepted (checksum-less)
}
