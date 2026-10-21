#include "doctest/doctest.h"
#include "fixtures.hpp"
#include "ss/persist/session.hpp"

using namespace ss;
using namespace ss::persist;

static const char* kPath = "/shows/set.show";

namespace {
struct Env {
    MemFileSystem fs;
    FakeClock clock;
    ShowSession session{fs, clock, kPath};
    int snapshots = 0;
    Env() { fs.createDirs("/shows"); }
    bool tick(const std::string& name = "edited") {
        return session.tickAutosave([&](model::ShowDocument& d, JsonWriteOptions&) { ++snapshots; d = fx::sampleDoc(name); });
    }
};
}  // namespace

TEST_CASE("REQ-X01: autosave waits 30 s after the first change and then writes only the recovery file") {
    Env e;
    CHECK_FALSE(e.tick());                         // clean: nothing to do
    e.session.noteChanged();
    e.clock.advance(secToNs(29));
    CHECK_FALSE(e.tick());
    CHECK(e.snapshots == 0);                       // snapshot is only taken when due
    e.clock.advance(secToNs(1));
    CHECK(e.tick("autosaved"));
    CHECK(e.snapshots == 1);
    CHECK(e.fs.exists(e.session.store().recoveryPath()));
    CHECK_FALSE(e.fs.exists(kPath));               // the user's file is never touched by autosave
    CHECK(e.session.dirty());                      // still unsaved relative to the user's file
    CHECK_FALSE(e.session.autosavePending());
    CHECK_FALSE(e.tick());
}

TEST_CASE("REQ-X01: more edits do not push the autosave deadline out (at most one write per 30 s)") {
    Env e;
    e.session.noteChanged();
    for (int i = 0; i < 29; ++i) { e.clock.advance(secToNs(1)); e.session.noteChanged(); CHECK_FALSE(e.tick()); }
    e.clock.advance(secToNs(1));
    CHECK(e.tick());
    e.session.noteChanged();
    e.clock.advance(secToNs(10));
    CHECK_FALSE(e.tick());                         // new window starts at the next change
    e.clock.advance(secToNs(20));
    CHECK(e.tick());
}

TEST_CASE("REQ-X01: failed autosave keeps the document dirty, keeps the recovery file, and retries after another window") {
    Env e;
    e.session.noteChanged();
    e.clock.advance(secToNs(30));
    REQUIRE(e.tick("first"));
    const std::string firstRecovery = e.fs.get(e.session.store().recoveryPath());
    e.session.noteChanged();
    e.clock.advance(secToNs(30));
    e.fs.resetOpCount();
    MemFileSystem::Fault f; f.atOp = 1; f.kind = MemFileSystem::FaultKind::DiskFull;
    e.fs.setFault(f);
    REQUIRE(e.tick("second"));
    REQUIRE(e.session.lastError().has_value());
    CHECK(e.session.lastError()->category == ErrorCategory::DiskFull);
    CHECK(e.session.autosavePending());
    CHECK(e.fs.get(e.session.store().recoveryPath()) == firstRecovery);   // old recovery file preserved
    e.fs.clearFault();
    CHECK_FALSE(e.tick());                          // not before the next window
    e.clock.advance(secToNs(30));
    CHECK(e.tick("second"));
    CHECK_FALSE(e.session.lastError().has_value());
    CHECK_FALSE(e.session.autosavePending());
}

TEST_CASE("REQ-X01: explicit save failure preserves dirty state and the old file, with a visible error") {
    Env e;
    REQUIRE(e.session.saveNow(fx::sampleDoc("v1")).ok());
    e.session.noteChanged();
    CHECK(e.session.dirty());
    e.fs.resetOpCount();
    MemFileSystem::Fault f; f.atOp = 4; f.kind = MemFileSystem::FaultKind::DiskFull;   // ops 1-3 are stale-temp cleanup; 4 writes the temp file
    e.fs.setFault(f);
    const auto st = e.session.saveNow(fx::sampleDoc("v2"));
    REQUIRE_FALSE(st.ok());
    CHECK(e.session.dirty());
    REQUIRE(e.session.lastError().has_value());
    CHECK(e.session.lastError()->category == ErrorCategory::DiskFull);
    e.fs.clearFault();
    CHECK(e.session.store().load().value().parsed.doc.name == "v1");
    REQUIRE(e.session.saveNow(fx::sampleDoc("v2")).ok());
    CHECK_FALSE(e.session.dirty());
    CHECK_FALSE(e.session.lastError().has_value());
    CHECK(e.session.store().load().value().parsed.doc.name == "v2");
}

TEST_CASE("REQ-X01: crash recovery proposes a disarmed restored copy and never applies it automatically") {
    Env e;
    REQUIRE(e.session.saveNow(fx::sampleDoc("saved")).ok());
    REQUIRE(e.session.open().ok());
    e.session.noteChanged();
    e.clock.advance(secToNs(30));
    REQUIRE(e.tick("unsaved work"));
    // --- crash: new process, same files ---
    ShowSession next(e.fs, e.clock, kPath);
    auto prop = next.detectRecovery();
    REQUIRE(prop.ok());
    REQUIRE(prop.value().has_value());
    const auto& p = *prop.value();
    CHECK(p.showName == "unsaved work");
    CHECK(p.uncleanShutdown);
    CHECK(p.newerThanSavedFile);
    CHECK(p.startDisarmed);
    CHECK_FALSE(p.lightingArmed);
    CHECK_FALSE(p.broadcastLive);
    CHECK_FALSE(p.recordingActive);
    CHECK(p.restoredCopyPath != kPath);
    // the user's file is untouched until they decide
    CHECK(next.store().load().value().parsed.doc.name == "saved");
    auto restored = next.acceptRestore(p);
    REQUIRE(restored.ok());
    CHECK(restored.value().doc.name == "unsaved work");
    ShowStore copy(e.fs, e.clock, p.restoredCopyPath);
    CHECK(copy.load().value().parsed.doc.name == "unsaved work");
    CHECK(next.store().load().value().parsed.doc.name == "saved");   // original still not overwritten
    CHECK_FALSE(e.fs.exists(next.store().recoveryPath()));
    CHECK_FALSE(e.fs.exists(next.store().sessionLockPath()));
    // nothing left to propose
    CHECK_FALSE(next.detectRecovery().value().has_value());
}

TEST_CASE("REQ-X01: discarding a recovery proposal removes the autosave") {
    Env e;
    REQUIRE(e.session.open().ok());
    e.session.noteChanged();
    e.clock.advance(secToNs(30));
    REQUIRE(e.tick("lost"));
    ShowSession next(e.fs, e.clock, kPath);
    auto prop = next.detectRecovery();
    REQUIRE(prop.value().has_value());
    CHECK(prop.value()->savedWallUnixMs == 0);     // no saved file at all
    REQUIRE(next.discardRestore().ok());
    CHECK_FALSE(next.detectRecovery().value().has_value());
}

TEST_CASE("REQ-X01: torn or damaged autosave files are ignored, the saved file stays authoritative") {
    Env e;
    REQUIRE(e.session.saveNow(fx::sampleDoc("saved")).ok());
    REQUIRE(e.session.open().ok());
    e.fs.put(e.session.store().recoveryPath(), "STAGESIGNAL-SAVE 1 kind=autosave saved=99 bytes=999 sha256=00\n{");
    ShowSession next(e.fs, e.clock, kPath);
    CHECK_FALSE(next.detectRecovery().value().has_value());
    CHECK(next.store().load().value().parsed.doc.name == "saved");
}

TEST_CASE("REQ-X01: explicit save supersedes the autosave; clean exit leaves nothing to recover") {
    Env e;
    REQUIRE(e.session.open().ok());
    e.session.noteChanged();
    e.clock.advance(secToNs(30));
    REQUIRE(e.tick("auto"));
    REQUIRE(e.fs.exists(e.session.store().recoveryPath()));
    REQUIRE(e.session.saveNow(fx::sampleDoc("final")).ok());
    CHECK_FALSE(e.fs.exists(e.session.store().recoveryPath()));
    REQUIRE(e.session.closeClean().ok());
    ShowSession next(e.fs, e.clock, kPath);
    CHECK_FALSE(next.detectRecovery().value().has_value());
}

TEST_CASE("REQ-X01: a stale recovery older than the saved file is not offered after a clean session") {
    Env e;
    e.clock.advance(secToNs(5));
    REQUIRE(e.session.store().writeRecovery(fx::sampleDoc("old auto")).ok());
    e.clock.advance(secToNs(60));
    REQUIRE(e.session.store().save(fx::sampleDoc("saved later")).ok());
    // simulate a recovery file that somehow survived (older than the save)
    e.fs.put(e.session.store().recoveryPath(), makeEnvelope(showToJson(fx::sampleDoc("old auto")).value(), "autosave", 1));
    ShowSession next(e.fs, e.clock, kPath);
    CHECK_FALSE(next.detectRecovery().value().has_value());
}
