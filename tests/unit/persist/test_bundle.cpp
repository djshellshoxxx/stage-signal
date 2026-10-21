#include "doctest/doctest.h"
#include <map>
#include "fixtures.hpp"
#include "nlohmann/json.hpp"
#include "ss/base/sha256.hpp"
#include "ss/persist/bundle.hpp"

using namespace ss;
using namespace ss::persist;
using nlohmann::json;

static bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

static BundleChoices all() { BundleChoices c; c.includeAllMedia = true; return c; }

TEST_CASE("REQ-S13: portable bundle plan has manifest.json, show.json and content-addressed media only") {
    const auto doc = fx::sampleDoc();
    const auto plan = planBundle(doc, all(), "0.1.0");
    REQUIRE(plan.ok());
    const auto& m = plan.value().members;
    REQUIRE(m.size() == 3);
    CHECK(m[0].path == "manifest.json");
    CHECK(m[1].path == "show.json");
    CHECK(m[2].path == "media/" + doc.media[0].sha256 + ".mp4");
    CHECK(m[2].content.empty());
    CHECK(m[2].size == 10);
    CHECK(plan.value().totalBytes == m[0].size + m[1].size + m[2].size);
    for (const auto& x : m) CHECK(isAllowedBundlePath(x.path));
    // show.json refers to the content-addressed path
    CHECK(has(m[1].content, "media/" + doc.media[0].sha256 + ".mp4"));
    CHECK_FALSE(has(m[1].content, "media/loop.mp4"));
}

TEST_CASE("IF-11: portable bundle excludes physical identifiers, local destinations and secrets") {
    auto doc = fx::sampleDoc();
    doc.journal.push_back({1, 1, "cast", "rtmps://u:p@live.example.com/app/abcd1234efgh5678ijkl"});
    const auto plan = planBundle(doc, all());
    REQUIRE(plan.ok());
    for (const auto& m : plan.value().members) {
        CHECK_FALSE(has(m.content, "SERIAL0001"));
        CHECK_FALSE(has(m.content, "192.168.1.50"));
        CHECK_FALSE(has(m.content, "abcd1234efgh5678ijkl"));
        CHECK_FALSE(has(m.content, "u:p@"));
    }
}

TEST_CASE("REQ-S13: media inclusion prompts are data; excluded media warns about affected scenes and cues") {
    const auto doc = fx::sampleDoc();
    const auto prompts = mediaPrompts(doc);
    REQUIRE(prompts.size() == 1);
    CHECK(prompts[0].mediaId == 7);
    CHECK(prompts[0].fileLabel == "loop.mp4");
    CHECK(prompts[0].usedByScenes == std::vector<std::uint32_t>{2});
    CHECK(prompts[0].usedByCues == std::vector<std::uint32_t>{2});
    CHECK(prompts[0].flashRisk);
    CHECK(has(prompts[0].question, "loop.mp4"));
    CHECK(has(prompts[0].question, "10.0 B"));

    const auto plan = planBundle(doc, BundleChoices{});   // nothing chosen
    REQUIRE(plan.ok());
    CHECK(plan.value().members.size() == 2);
    CHECK(plan.value().excludedMedia == std::vector<std::uint32_t>{7});
    bool scene = false, cue = false;
    for (const auto& w : plan.value().warnings) { scene |= has(w, "scene 2"); cue |= has(w, "cue 2"); }
    CHECK(scene);
    CHECK(cue);
    // imports back with media flagged as not included
    auto man = parseManifest(plan.value().members[0].content);
    REQUIRE(man.ok());
    auto imp = importBundleShow(plan.value().members[1].content, man.value());
    REQUIRE(imp.ok());
    CHECK(imp.value().doc.media[0].relativePath.empty());
    CHECK_FALSE(imp.value().warnings.empty());
}

TEST_CASE("REQ-S13: identical media content is stored once") {
    auto doc = fx::sampleDoc();
    auto second = doc.media[0];
    second.id = 8; second.relativePath = "media/copy.mp4";
    doc.media.push_back(second);
    const auto plan = planBundle(doc, all());
    REQUIRE(plan.ok());
    CHECK(plan.value().members.size() == 3);
}

TEST_CASE("REQ-S13: planner warns about fields that look like secrets, without echoing them") {
    auto doc = fx::sampleDoc();
    doc.messages[0].utf8 = "wifi token=abcdef123456";
    const auto plan = planBundle(doc, all());
    REQUIRE(plan.ok());
    bool warned = false;
    for (const auto& w : plan.value().warnings) { warned |= has(w, "messages[0].utf8"); CHECK_FALSE(has(w, "abcdef123456")); }
    CHECK(warned);
}

TEST_CASE("REQ-S13: relocated bundle imports offline and matches the original (export -> import round trip)") {
    const auto doc = fx::sampleDoc();
    const auto plan = planBundle(doc, all());
    REQUIRE(plan.ok());
    // pretend this is the extracted bundle on another machine
    std::map<std::string, std::string> files;
    for (const auto& m : plan.value().members) files[m.path] = m.content;
    // listing -> validation
    std::vector<security::ArchiveEntryInfo> listing;
    for (const auto& m : plan.value().members) { security::ArchiveEntryInfo e; e.path = m.path; e.uncompressedSize = m.size; e.compressedSize = m.size; listing.push_back(e); }
    REQUIRE(validateBundleListing(listing).ok());
    auto man = parseManifest(files["manifest.json"]);
    REQUIRE(man.ok());
    CHECK(man.value().showUuid == doc.uuid);
    for (const auto& m : plan.value().members) {
        if (m.kind == MemberKind::Manifest) continue;
        const std::string content = m.kind == MemberKind::Show ? m.content : std::string(10, 'x');
        // media bytes in this synthetic test do not hash to the manifest value -> mismatch is caught
        const auto st = checkMemberAgainstManifest(man.value(), m.path, content.size(), Sha256::hashHex(content));
        if (m.kind == MemberKind::Show) CHECK(st.ok()); else CHECK_FALSE(st.ok());
    }
    auto imp = importBundleShow(files["show.json"], man.value());
    REQUIRE(imp.ok());
    const auto& d = imp.value().doc;
    CHECK(d.name == doc.name);
    CHECK(d.scenes.size() == doc.scenes.size());
    CHECK(d.cues.size() == doc.cues.size());
    CHECK(d.midi[0].actionId == doc.midi[0].actionId);
    CHECK(d.media[0].relativePath == "media/" + doc.media[0].sha256 + ".mp4");
    CHECK(d.midi[0].deviceIdentity.empty());
    CHECK(d.seed == doc.seed);
}

TEST_CASE("REQ-S13: media bytes are verified against the recorded hash") {
    const auto doc = fx::sampleDoc();
    CHECK(verifyMediaBytes(doc.media[0], "loop-bytes", 10).ok());
    CHECK_FALSE(verifyMediaBytes(doc.media[0], "loop-bytez", 10).ok());
    CHECK_FALSE(verifyMediaBytes(doc.media[0], "loop-bytes!", 11).ok());
}

TEST_CASE("REQ-S14: hostile bundle listings are rejected before any extraction") {
    const std::string h = std::string(64, 'a');
    auto entry = [](const std::string& p, std::uint64_t u = 100, std::uint64_t c = 100) { security::ArchiveEntryInfo e; e.path = p; e.uncompressedSize = u; e.compressedSize = c; return e; };
    const std::vector<security::ArchiveEntryInfo> good{entry("manifest.json"), entry("show.json"), entry("media/" + h + ".png")};
    CHECK(validateBundleListing(good).ok());

    SUBCASE("traversal and absolute members") {
        for (const char* p : {"../evil", "/etc/passwd", "C:/x", "\\\\h\\s\\x", "media/../../x", "media\\x.png"}) {
            auto l = good; l.push_back(entry(p));
            INFO(p);
            CHECK_FALSE(validateBundleListing(l).ok());
        }
    }
    SUBCASE("members outside the allowlist") {
        for (const char* p : {"readme.txt", "media/notahash.png", "media/sub/x.png", "script.lua", "show.json.bak"}) {
            auto l = good; l.push_back(entry(p));
            INFO(p);
            CHECK_FALSE(validateBundleListing(l).ok());
        }
        auto l = good; l.push_back(entry("media/" + h + ".exe"));
        CHECK_FALSE(validateBundleListing(l).ok());
    }
    SUBCASE("duplicate canonical path by case") {
        auto l = good; l.push_back(entry("SHOW.JSON"));
        CHECK_FALSE(validateBundleListing(l).ok());
    }
    SUBCASE("symlink flag") {
        auto l = good; l[2].isSymlinkOrReparse = true;
        CHECK_FALSE(validateBundleListing(l).ok());
    }
    SUBCASE("missing required members") {
        CHECK_FALSE(validateBundleListing({entry("show.json")}).ok());
        CHECK_FALSE(validateBundleListing({entry("manifest.json")}).ok());
    }
    SUBCASE("entry count over 10000") {
        std::vector<security::ArchiveEntryInfo> l;
        for (int i = 0; i < 10001; ++i) l.push_back(entry("x"));
        CHECK_FALSE(validateBundleListing(l).ok());
    }
    SUBCASE("zip bomb: huge uncompressed size from a tiny entry") {
        auto l = good; l[2].uncompressedSize = 1ull << 30; l[2].compressedSize = 1000;
        CHECK_FALSE(validateBundleListing(l).ok());
        l = good; l[1].uncompressedSize = model::Limits::kMaxJsonBytes + 1; l[1].compressedSize = l[1].uncompressedSize;
        CHECK_FALSE(validateBundleListing(l).ok());
        l = good; l[2].uncompressedSize = 3ull << 30; l[2].compressedSize = 3ull << 30;
        CHECK_FALSE(validateBundleListing(l).ok());
    }
}

TEST_CASE("REQ-S14: tampered manifest or show.json is rejected on import") {
    const auto plan = planBundle(fx::sampleDoc(), all());
    REQUIRE(plan.ok());
    auto man = parseManifest(plan.value().members[0].content);
    REQUIRE(man.ok());
    SUBCASE("media reference pointing at a different member") {
        auto j = json::parse(plan.value().members[1].content);
        j["media"][0]["relativePath"] = "show.json";
        CHECK_FALSE(importBundleShow(j.dump(), man.value()).ok());
        j["media"][0]["relativePath"] = "media/" + std::string(64, 'b') + ".mp4";
        CHECK_FALSE(importBundleShow(j.dump(), man.value()).ok());
    }
    SUBCASE("show.json from a different show") {
        auto other = fx::sampleDoc(); other.uuid = "00000000-0000-4000-8000-000000000000";
        CHECK_FALSE(importBundleShow(showToJson(other).value(), man.value()).ok());
    }
    SUBCASE("manifest problems") {
        CHECK_FALSE(parseManifest("nope").ok());
        auto m = json::parse(plan.value().members[0].content);
        m["bundleVersion"] = 99;
        CHECK_FALSE(parseManifest(m.dump()).ok());
        m = json::parse(plan.value().members[0].content);
        m["members"][0]["path"] = "../x";
        CHECK_FALSE(parseManifest(m.dump()).ok());
        m = json::parse(plan.value().members[0].content);
        m["format"] = "zip";
        CHECK_FALSE(parseManifest(m.dump()).ok());
        CHECK_FALSE(parseManifest(std::string(2u << 20, ' ')).ok());
    }
}

TEST_CASE("REQ-S13: bundle path allowlist") {
    const std::string h = std::string(64, '0');
    CHECK(isAllowedBundlePath("show.json"));
    CHECK(isAllowedBundlePath("media/" + h + ".gif"));
    CHECK_FALSE(isAllowedBundlePath("media/" + h + ".GIF"));
    CHECK_FALSE(isAllowedBundlePath("media/" + h.substr(1) + ".png"));
    CHECK_FALSE(isAllowedBundlePath("media/" + h + ".png.exe"));
    CHECK_FALSE(isAllowedBundlePath("other/" + h + ".png"));
}
