#include "doctest/doctest.h"
#include <algorithm>
#include <map>
#include "ss/diag/logrotate.hpp"
#include "ss/diag/support_bundle.hpp"
#include "ss/diag/text.hpp"
#include "ss/security/secret_store.hpp"

using namespace ss;
using namespace ss::diag;

static bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

// ---- log rotation ------------------------------------------------------------------------------
namespace {
struct FakeLogDir {
    std::map<std::string, std::uint64_t> files;
    LogRotationConfig cfg;
    std::vector<LogFileInfo> list() const { std::vector<LogFileInfo> v; for (auto& kv : files) v.push_back({kv.first, kv.second, 0}); return v; }
    void apply(const std::vector<RotateOp>& ops) {
        for (const auto& op : ops) {
            if (op.kind == RotateOp::Kind::Delete) files.erase(op.from);
            else { files[op.to] = files[op.from]; files.erase(op.from); }
        }
    }
    void append(std::uint64_t bytes) {
        const auto plan = planRotation(list(), cfg, bytes);
        apply(plan.ops);
        files[plan.activeName] += bytes;
    }
    std::uint64_t total() const { std::uint64_t t = 0; for (auto& kv : files) t += kv.second; return t; }
};
}  // namespace

TEST_CASE("REQ-B03: logs rotate at 5 files x 5 MiB and never exceed the budget") {
    FakeLogDir d;
    CHECK(d.cfg.maxFiles == 5);
    CHECK(d.cfg.maxBytes == 5u * 1024 * 1024);
    CHECK(d.cfg.totalBudget() == 25u * 1024 * 1024);
    const std::uint64_t line = 1000;
    for (std::uint64_t written = 0; written < 100ull * 1024 * 1024; written += line) {
        d.append(line);
        CHECK(d.files.size() <= 5);
        CHECK(d.total() <= d.cfg.totalBudget());
    }
    CHECK(d.files.size() == 5);
    for (int i = 0; i < 5; ++i) CHECK(d.files.count(logFileName(d.cfg, i)) == 1);
    for (auto& kv : d.files) CHECK(kv.second <= d.cfg.maxBytes);
}

TEST_CASE("REQ-B03: rotation plan details") {
    LogRotationConfig cfg;
    // under the limit: nothing to do
    CHECK_FALSE(planRotation({{"stagesignal.log", 100, 0}}, cfg, 100).rotate);
    CHECK_FALSE(planRotation({}, cfg, 100).rotate);
    // at the limit with all five files present: delete .4, shift .3->.4 ... active->.1
    std::vector<LogFileInfo> all;
    for (int i = 0; i < 5; ++i) all.push_back({logFileName(cfg, i), cfg.maxBytes, 0});
    const auto p = planRotation(all, cfg, 10);
    REQUIRE(p.rotate);
    REQUIRE(p.ops.size() == 5);
    CHECK(p.ops[0].kind == RotateOp::Kind::Delete);
    CHECK(p.ops[0].from == "stagesignal.4.log");
    CHECK(p.ops[1].from == "stagesignal.3.log");
    CHECK(p.ops[1].to == "stagesignal.4.log");
    CHECK(p.ops[4].from == "stagesignal.log");
    CHECK(p.ops[4].to == "stagesignal.1.log");
    CHECK(p.activeName == "stagesignal.log");
    // gaps in the numbering are fine
    const auto q = planRotation({{"stagesignal.log", cfg.maxBytes, 0}, {"stagesignal.3.log", 5, 0}}, cfg, 1);
    REQUIRE(q.rotate);
    CHECK(q.ops.size() == 2);
    // stray files beyond the limit are removed even without rotating
    const auto r = planRotation({{"stagesignal.log", 10, 0}, {"stagesignal.9.log", 5, 0}, {"other.log", 99, 0}}, cfg, 1);
    CHECK_FALSE(r.rotate);
    REQUIRE(r.ops.size() == 1);
    CHECK(r.ops[0].from == "stagesignal.9.log");
    // single-file config just drops the file
    LogRotationConfig one; one.maxFiles = 1;
    const auto s = planRotation({{"stagesignal.log", one.maxBytes, 0}}, one, 1);
    REQUIRE(s.rotate);
    REQUIRE(s.ops.size() == 1);
    CHECK(s.ops[0].kind == RotateOp::Kind::Delete);
}

TEST_CASE("REQ-B03: log file names are recognised strictly") {
    LogRotationConfig cfg;
    CHECK(parseLogIndex(cfg, "stagesignal.log") == 0);
    CHECK(parseLogIndex(cfg, "stagesignal.3.log") == 3);
    for (const char* bad : {"stagesignal.log.1", "stagesignal..log", "stagesignal.x.log", "stagesignal.01.log", "stagesignal.0.log", "other.log", "stagesignal", "", "stagesignal.-1.log"})
        CHECK(parseLogIndex(cfg, bad) == -1);
}

TEST_CASE("REQ-B03: retention and delete-all controls only touch our own log files") {
    LogRotationConfig cfg;
    const std::int64_t day = 86400000LL;
    const std::int64_t now = 100 * day;
    std::vector<LogFileInfo> f{{"stagesignal.log", 1, now}, {"stagesignal.1.log", 1, now - 10 * day}, {"stagesignal.2.log", 1, now - 40 * day}, {"notes.txt", 1, 0}};
    const auto old = planRetention(f, cfg, now, 30);
    REQUIRE(old.size() == 1);
    CHECK(old[0].from == "stagesignal.2.log");
    const auto del = planDeleteAll(f, cfg);
    CHECK(del.size() == 3);
    for (const auto& op : del) CHECK(op.from != "notes.txt");
}

TEST_CASE("REQ-B03: log lines are redacted, single-line and bounded") {
    const auto line = formatLogLine(1'700'000'000'123LL, "WARN", "connect failed rtmps://u:p@h.example.com/app/abcd1234efgh5678ijkl\nsecond line password=hunter2 /Users/Sam/x");
    CHECK(line.rfind("2023-11-14T22:13:20.123Z WARN ", 0) == 0);
    CHECK(line.back() == '\n');
    CHECK(std::count(line.begin(), line.end(), '\n') == 1);
    CHECK_FALSE(has(line, "u:p@"));
    CHECK_FALSE(has(line, "abcd1234efgh5678ijkl"));
    CHECK_FALSE(has(line, "hunter2"));
    CHECK_FALSE(has(line, "/Users/Sam"));
    CHECK(formatLogLine(0, "BOGUS", "x").find(" INFO x") != std::string::npos);
    CHECK(formatLogLine(0, "INFO", std::string(100000, 'a')).size() < 2100);
}

// ---- support bundle ----------------------------------------------------------------------------
TEST_CASE("REQ-B03: support bundle never accepts raw audio, camera frames, key material, full media or crash dumps") {
    SupportBundleComposer c;
    for (auto cat : {BundleCategory::RawAudio, BundleCategory::CameraFrames, BundleCategory::KeyMaterial, BundleCategory::FullMedia, BundleCategory::CrashDump}) {
        CHECK(isForbidden(cat));
        const auto st = c.addItem(cat, "x.txt", "hello");
        REQUIRE_FALSE(st.ok());
        CHECK(st.error().category == ErrorCategory::PermissionDenied);
    }
    for (const char* name : {"audio.wav", "cam.PNG", "clip.mp4", "crash.dmp", "server.pem", "id.key", "store.pfx", "x/y.flac", "a.zip"})
        CHECK_FALSE(c.addItem(BundleCategory::Logs, name, "text").ok());
    CHECK_FALSE(c.addItem(BundleCategory::Logs, "ok.log", std::string("RIFF\0\0\0\0WAVEfmt ", 16)).ok());
    CHECK_FALSE(c.addItem(BundleCategory::Logs, "ok.log", std::string("\x89PNG\r\n", 6)).ok());
    CHECK_FALSE(c.addItem(BundleCategory::Logs, "ok.log", "-----BEGIN PRIVATE KEY-----\nMIIE...\n-----END PRIVATE KEY-----").ok());
    CHECK_FALSE(c.addItem(BundleCategory::Logs, "ok.log", "log before\n-----BEGIN CERTIFICATE-----\nAAAA").ok());
    CHECK_FALSE(c.addItem(BundleCategory::Logs, "ok.log", std::string("a\0b", 3)).ok());
    CHECK_FALSE(c.addItem(BundleCategory::Logs, "ok.log", "bad \xff utf8").ok());
    CHECK_FALSE(c.addItem(BundleCategory::Logs, "../escape.log", "x").ok());
    CHECK_FALSE(c.addItem(BundleCategory::Logs, "/abs.log", "x").ok());
    CHECK_FALSE(c.addItem(BundleCategory::Logs, "manifest.txt", "x").ok());
    CHECK_FALSE(c.addItem(BundleCategory::Logs, "big.log", std::string(kBundleMaxItemBytes + 1, 'a')).ok());
    CHECK(c.addItem(BundleCategory::Logs, "ok.log", "fine").ok());
    CHECK_FALSE(c.addItem(BundleCategory::Logs, "ok.log", "dup").ok());
    CHECK(c.preview().size() == 1);
}

TEST_CASE("REQ-B03: support bundle previews categories, redaction counts and defaults before composing") {
    SupportBundleComposer c;
    REQUIRE(c.addItem(BundleCategory::AppInfo, "app.txt", "StageSignal 0.1.0").ok());
    REQUIRE(c.addItem(BundleCategory::Logs, "stagesignal.log", "2026-10-20 connect rtmps://u:p@h/app/abcd1234efgh5678ijkl\nsecond\nthird\nfourth password=hunter2").ok());
    REQUIRE(c.addItem(BundleCategory::ShowStructure, "show-structure.json", "{\"scenes\":3}").ok());
    const auto pv = c.preview();
    REQUIRE(pv.size() == 3);
    CHECK(pv[0].category == BundleCategory::AppInfo);
    CHECK(pv[0].selected);
    CHECK(pv[1].category == BundleCategory::Logs);
    CHECK(pv[1].redactions >= 3);
    REQUIRE(pv[1].sampleLines.size() == 3);
    CHECK_FALSE(has(pv[1].sampleLines[0], "u:p@"));
    CHECK(pv[2].category == BundleCategory::ShowStructure);
    CHECK_FALSE(pv[2].selected);          // opt-in
    CHECK(pv[1].itemNames == std::vector<std::string>{"stagesignal.log"});
    for (const auto& p : pv) { CHECK_FALSE(p.label.empty()); CHECK_FALSE(p.description.empty()); }
}

TEST_CASE("REQ-B03: composed support bundle contains only selected categories, redacted, plus a manifest") {
    SupportBundleComposer c;
    security::InMemorySecretStore store;
    REQUIRE(store.put("secret:yt", "xK9-literal-stream-key-77").ok());
    c.setRedactor(security::makeRedactorFor(store));
    REQUIRE(c.addItem(BundleCategory::EventJournal, "journal.csv", "1,cast,start with xK9-literal-stream-key-77\n2,cast,stop").ok());
    REQUIRE(c.addItem(BundleCategory::Logs, "stagesignal.log", "token=abc12345 /home/sheldon/shows\nrtmps://live.example.com/app/abcd1234efgh5678ijkl").ok());
    REQUIRE(c.addItem(BundleCategory::ShowStructure, "show.json", "{\"name\":\"x\"}").ok());
    const auto b = c.compose();
    REQUIRE(b.ok());
    std::vector<std::string> names;
    for (const auto& f : b.value().files) names.push_back(f.name);
    CHECK(names == std::vector<std::string>{"manifest.txt", "journal.csv", "stagesignal.log"});   // ShowStructure not selected
    for (const auto& f : b.value().files) {
        CHECK_FALSE(has(f.content, "xK9-literal-stream-key-77"));
        CHECK_FALSE(has(f.content, "abc12345"));
        CHECK_FALSE(has(f.content, "abcd1234efgh5678ijkl"));
        CHECK_FALSE(has(f.content, "/home/sheldon"));
    }
    CHECK(b.value().totalRedactions >= 4);
    CHECK(has(b.value().files[0].content, "Event journal"));
    CHECK_FALSE(has(b.value().files[0].content, "Show structure"));
    CHECK(has(b.value().files[0].content, "Never included: raw audio, camera frames"));
    security::Redactor r = security::makeRedactorFor(store);
    CHECK(scanBundleForSecrets(b.value(), r).empty());

    // flipping selection changes the output
    c.setSelected(BundleCategory::ShowStructure, true);
    c.setSelected(BundleCategory::Logs, false);
    const auto b2 = c.compose();
    REQUIRE(b2.ok());
    CHECK(b2.value().files.size() == 3);
    CHECK(b2.value().files[2].name == "show.json");
    // forbidden categories cannot be selected
    c.setSelected(BundleCategory::RawAudio, true);
    for (const auto& p : c.preview()) CHECK(p.category != BundleCategory::RawAudio);
    // nothing selected -> error
    SupportBundleComposer empty;
    CHECK_FALSE(empty.compose().ok());
}

TEST_CASE("REQ-B03: support bundle respects total size limits") {
    SupportBundleComposer c;
    for (int i = 0; i < 5; ++i) REQUIRE(c.addItem(BundleCategory::Logs, "l" + std::to_string(i) + ".log", std::string(7u << 20, 'a')).ok());
    const auto r = c.compose();   // 35 MiB > 32 MiB
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().category == ErrorCategory::ResourceExhausted);
    SupportBundleComposer many;
    for (std::size_t i = 0; i < kBundleMaxItems; ++i) REQUIRE(many.addItem(BundleCategory::Logs, "f" + std::to_string(i), "x").ok());
    CHECK_FALSE(many.addItem(BundleCategory::Logs, "extra", "x").ok());
}

// ---- text helpers ------------------------------------------------------------------------------
TEST_CASE("REQ-X05: text helpers are locale-free and exact") {
    CHECK(formatUtcIso(0) == "1970-01-01T00:00:00Z");
    CHECK(formatUtcIso(1'700'000'000'123LL, true) == "2023-11-14T22:13:20.123Z");
    CHECK(formatUtcIso(951'782'400'000LL) == "2000-02-29T00:00:00Z");
    CHECK(formatUtcIso(-1) == "1969-12-31T23:59:59Z");
    CHECK(formatUtcIso(4'102'444'800'000LL) == "2100-01-01T00:00:00Z");
    CHECK(formatDuration(0) == "0:00");
    CHECK(formatDuration(secToNs(65)) == "1:05");
    CHECK(formatDuration(secToNs(3725)) == "1:02:05");
    CHECK(formatDuration(-5) == "0:00");
    CHECK(fixedDecimals(1.25, 1) == "1.3");
    CHECK(fixedDecimals(128.0, 1) == "128.0");
    CHECK(fixedDecimals(0.05, 2) == "0.05");
    CHECK(fixedDecimals(-0.004, 2) == "0.00");
    CHECK(fixedDecimals(-2.5, 0) == "-3");
    CHECK(fixedDecimals(0.0 / 0.0, 2) == "0");
    CHECK(keyName(9, true) == "A minor");
    CHECK(keyName(1, false) == "C# major");
    CHECK(keyName(-1, false).empty());
    CHECK(htmlEscape("<a href=\"x\">&'`</a>") == "&lt;a href=&quot;x&quot;&gt;&amp;&#39;&#96;&lt;/a&gt;");
    CHECK(htmlEscape("a\x01" "b\xff") == "ab?");   // control dropped, invalid UTF-8 byte replaced
    CHECK(csvText("plain") == "plain");
    CHECK(csvText("a,b") == "\"a,b\"");
    CHECK(csvText("say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(csvText("=1+1") == "'=1+1");
    CHECK(csvText("@SUM(A1)") == "'@SUM(A1)");
    CHECK(csvText("-2+3") == "'-2+3");
    CHECK(csvText("two\nlines") == "\"two\nlines\"");
    CHECK(csvNumber("12.5") == "12.5");
    CHECK(csvNumber("=1") == "");
}
