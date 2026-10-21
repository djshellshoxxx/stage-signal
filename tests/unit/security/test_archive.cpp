#include "doctest/doctest.h"
#include <string>
#include <vector>
#include "ss/security/archive.hpp"

using namespace ss::security;

static ArchiveEntryInfo file(const std::string& p, std::uint64_t u = 10, std::uint64_t c = 10) {
    ArchiveEntryInfo e; e.path = p; e.uncompressedSize = u; e.compressedSize = c; return e;
}

TEST_CASE("REQ-S14: hostile member paths are rejected") {
    const std::string hostile[] = {
        "/etc/passwd", "C:/Windows/x", "c:\\x", "\\\\server\\share\\x", "//server/share/x", "../x", "a/../../x",
        "a/..", "a\\b", "..\\x", "a/./b", "a//b", "", "/", ".hidden", "a/.git/config", "file.txt:stream",
        "CON", "con.txt", "a/NUL.png", "COM1", "lpt9.dat", "name.", "a/b.", std::string("a\0b", 3),
        "a\x01" "b", "caf\xC3\xA9.png", "a b", "a?b", "a*b", "a|b", "media/%2e%2e/x",
    };
    for (const auto& p : hostile) {
        INFO(p);
        CHECK_FALSE(canonicalMemberPath(p).ok());
    }
    CHECK_FALSE(canonicalMemberPath(std::string(300, 'a')).ok());
    CHECK_FALSE(canonicalMemberPath("a/b/c/d/e/f/g/h/i/j").ok());   // depth
}

TEST_CASE("REQ-S14: legitimate bundle paths pass and are canonical") {
    CHECK(canonicalMemberPath("show.json").value() == "show.json");
    CHECK(canonicalMemberPath("media/0123abcd.png").value() == "media/0123abcd.png");
    CHECK(canonicalMemberPath("media/").value() == "media");
}

TEST_CASE("REQ-S14: error text never echoes the hostile path") {
    const auto r = canonicalMemberPath("../../secret-user-name/x");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().detail.find("secret-user-name") == std::string::npos);
}

TEST_CASE("REQ-S14: symlink or reparse flag from the caller is rejected") {
    auto e = file("media/a.png");
    e.isSymlinkOrReparse = true;
    CHECK_FALSE(validateArchive({e}).ok());
}

TEST_CASE("REQ-S14: duplicate canonical paths, including case-fold, are rejected") {
    CHECK_FALSE(validateArchive({file("media/A.png"), file("media/a.PNG")}).ok());
    CHECK_FALSE(validateArchive({file("show.json"), file("show.json")}).ok());
    CHECK_FALSE(validateArchive({file("media"), file("media/a.png")}).ok());   // file used as directory
    auto dir = file("media/", 0, 0); dir.isDirectory = true;
    CHECK(validateArchive({dir, file("media/a.png")}).ok());
    auto mismatch = file("media/"); // trailing slash but not flagged as directory
    CHECK_FALSE(validateArchive({mismatch}).ok());
}

TEST_CASE("REQ-S14: entry count limit is checked before per-entry work") {
    ArchiveLimits lim; lim.maxEntries = 3;
    std::vector<ArchiveEntryInfo> v;
    for (int i = 0; i < 4; ++i) v.push_back(file("a" + std::to_string(i)));
    v[0].path = "../evil";   // would also fail, but count must be reported first
    const auto r = validateArchive(v, lim);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().category == ss::ErrorCategory::ResourceExhausted);
    CHECK_FALSE(checkArchiveHeader(10001, 0, 0).ok());
    CHECK(checkArchiveHeader(10000, 1000, 1000).ok());
}

TEST_CASE("REQ-S14: size and compression-ratio bombs are rejected from headers alone") {
    // classic zip bomb header: 4 GiB claimed from 1 MiB
    CHECK_FALSE(validateArchive({file("media/a.png", 4ull << 30, 1u << 20)}).ok());
    // 1 GiB of zeros from 1 KiB
    CHECK_FALSE(validateArchive({file("media/b.png", 1ull << 30, 1024)}).ok());
    // zero compressed bytes but data claimed
    CHECK_FALSE(validateArchive({file("media/c.png", 1ull << 20, 0)}).ok());
    // many entries that individually pass but exceed the total
    ArchiveLimits lim; lim.maxTotalUncompressed = 1000;
    CHECK_FALSE(validateArchive({file("a", 600, 600), file("b", 600, 600)}, lim).ok());
    // overflow attempt on sums
    CHECK_FALSE(validateArchive({file("a", ~0ull, ~0ull), file("b", ~0ull, ~0ull)}).ok());
    CHECK_FALSE(checkArchiveHeader(1, 3ull << 30, 3ull << 30).ok());
    CHECK_FALSE(checkArchiveHeader(1, 10ull << 20, 10).ok());
    // small and honest passes; small highly-compressible entry is exempt below the floor
    CHECK(validateArchive({file("show.json", 5000, 100), file("media/a.png", 1 << 20, 1 << 20)}).ok());
}

TEST_CASE("REQ-S14: extraction budget stops a header that lies about size") {
    auto ents = validateArchive({file("a.bin", 100, 100), file("b.bin", 50, 50)});
    REQUIRE(ents.ok());
    ExtractionBudget b(ents.value(), ArchiveLimits{});
    CHECK_FALSE(b.charge(1).ok());   // no active entry
    CHECK(b.begin(0).ok());
    CHECK(b.charge(60).ok());
    CHECK(b.charge(40).ok());
    CHECK_FALSE(b.charge(1).ok());   // entry exceeded its declaration
    CHECK(b.begin(1).ok());
    CHECK(b.charge(50).ok());
    CHECK_FALSE(b.charge(1).ok());
    CHECK(b.totalWritten() == 150);
    CHECK_FALSE(b.begin(7).ok());
}
