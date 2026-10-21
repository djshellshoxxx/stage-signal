#include "doctest/doctest.h"
#include <string>
#include "fixtures.hpp"
#include "ss/base/utf8.hpp"
#include "ss/persist/show_json.hpp"

using namespace ss;
using namespace ss::persist;
using nlohmann::json;

static bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

TEST_CASE("REQ-S13: show document round-trips through JSON with every field") {
    const auto doc = fx::sampleDoc();
    const auto text = showToJson(doc);
    REQUIRE(text.ok());
    auto back = showFromJson(text.value());
    REQUIRE(back.ok());
    const auto& d = back.value().doc;
    CHECK(d.uuid == doc.uuid);
    CHECK(d.name == "Friday set");
    CHECK(d.seed == 18446744073709551615ull);
    CHECK(d.mode == ShowMode::Hybrid);
    CHECK(d.strobingDisabled);
    REQUIRE(d.scenes.size() == 2);
    CHECK(d.scenes[0].kind == model::SceneKind::NeonGrid);
    CHECK(d.scenes[0].seed == 0xFFFFFFFFFFFFFFFEull);
    CHECK(d.scenes[0].params == doc.scenes[0].params);
    CHECK(d.scenes[0].tier == model::EnergyTier::Drop);
    CHECK(d.scenes[1].strobe);
    CHECK(d.scenes[1].mediaId == 7);
    REQUIRE(d.cues.size() == 2);
    CHECK(d.cues[0].lightingRefs == std::vector<std::uint32_t>{1, 2});
    CHECK(d.cues[1].timeBase == model::TimeBase::Beats);
    CHECK(d.cues[1].waitForTrigger);
    REQUIRE(d.outputs.size() == 2);
    CHECK(d.outputs[1].kind == RouteKind::Duplicate);
    CHECK(d.outputs[1].duplicateOf == 1);
    REQUIRE(d.media.size() == 1);
    CHECK(d.media[0].sha256 == doc.media[0].sha256);
    CHECK(d.media[0].flashRisk);
    REQUIRE(d.messages.size() == 1);
    CHECK(d.messages[0].utf8 == doc.messages[0].utf8);
    CHECK(d.messages[0].color == model::Rgb{10, 20, 30});
    REQUIRE(d.midi.size() == 1);
    CHECK(d.midi[0].actionId == id(ActionId::NextScene));
    CHECK(d.midi[0].encoder == model::EncoderMode::RelativeTwosComplement);
    CHECK(d.midi[0].channel == 255);
    CHECK(d.fixtureProfiles[0].channels.size() == 4);
    CHECK(d.patches[0].profileId == "generic-rgb");
    CHECK(d.lighting.protocol == model::LightingProtocol::Sacn);
    CHECK(d.lighting.destination == "192.168.1.50");
    CHECK(d.quality.locked);
    CHECK(d.journal.size() == 2);
    CHECK(back.value().warnings.empty());
    // serialise -> parse -> serialise is a fixed point
    CHECK(showToJson(d).value() == text.value());
}

TEST_CASE("REQ-S13: serialisation is deterministic and compact") {
    const auto a = showToJson(fx::sampleDoc()).value();
    const auto b = showToJson(fx::sampleDoc()).value();
    CHECK(a == b);
    CHECK(a.find('\n') == std::string::npos);
}

TEST_CASE("REQ-S13: unknown optional metadata and migration history are preserved") {
    JsonWriteOptions opt;
    opt.extras.metadataJson = R"({"thirdParty":{"note":"keep me","n":[1,2,3]}})";
    opt.extras.migrationHistory = {"1->2"};
    const auto text = showToJson(fx::sampleDoc(), opt);
    REQUIRE(text.ok());
    auto back = showFromJson(text.value());
    REQUIRE(back.ok());
    CHECK(json::parse(back.value().extras.metadataJson) == json::parse(opt.extras.metadataJson));
    CHECK(back.value().extras.migrationHistory == opt.extras.migrationHistory);
    // and survives another round
    JsonWriteOptions again; again.extras = back.value().extras;
    CHECK(showToJson(back.value().doc, again).value() == text.value());
}

TEST_CASE("REQ-S14: newer schema major is rejected with SchemaUnsupported") {
    auto j = fx::toJ(fx::sampleDoc());
    j["schema"]["major"] = 2;
    const auto r = showFromJson(j.dump());
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().category == ErrorCategory::SchemaUnsupported);
}

TEST_CASE("REQ-S13: newer minor loads and reports unsupported fields without values") {
    auto j = fx::toJ(fx::sampleDoc());
    j["schema"]["minor"] = 9;
    j["futureThing"] = "secret-looking-value-123";
    j["scenes"][0]["futureField"] = 5;
    const auto r = showFromJson(j.dump());
    REQUIRE(r.ok());
    CHECK(r.value().doc.schemaMinor == 9);
    CHECK(r.value().warnings.size() >= 3);
    for (const auto& w : r.value().warnings) CHECK_FALSE(has(w, "secret-looking-value"));
}

TEST_CASE("REQ-S14: unknown executable behaviour is rejected") {
    SUBCASE("executable-looking key at top level") { auto j = fx::toJ(fx::sampleDoc()); j["script"] = "alert(1)"; CHECK_FALSE(showFromJson(j.dump()).ok()); }
    SUBCASE("executable key inside a scene") { auto j = fx::toJ(fx::sampleDoc()); j["scenes"][1]["onLoad"] = "x"; CHECK_FALSE(showFromJson(j.dump()).ok()); }
    SUBCASE("executable key nested in metadata") {
        auto j = fx::toJ(fx::sampleDoc()); j["metadata"] = {{"a", {{"b", json::array({{{"exec", "calc"}}})}}}};
        CHECK_FALSE(showFromJson(j.dump()).ok());
    }
    SUBCASE("unknown scene kind (unknown renderer behaviour)") { auto j = fx::toJ(fx::sampleDoc()); j["scenes"][0]["kind"] = "shaderProgram"; CHECK_FALSE(showFromJson(j.dump()).ok()); }
    SUBCASE("unknown midi action id") { auto j = fx::toJ(fx::sampleDoc()); j["midi"][0]["actionId"] = 99999; CHECK_FALSE(showFromJson(j.dump()).ok()); }
    SUBCASE("unknown transition") { auto j = fx::toJ(fx::sampleDoc()); j["cues"][0]["transition"] = "morph"; CHECK_FALSE(showFromJson(j.dump()).ok()); }
}

TEST_CASE("REQ-S14: structural and limit violations are rejected before state is produced") {
    SUBCASE("size over 8 MiB is refused before parsing") {
        const std::string big(model::Limits::kMaxJsonBytes + 1, ' ');
        const auto r = showFromJson(big);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().category == ErrorCategory::ResourceExhausted);
    }
    SUBCASE("nesting bomb") {
        const std::string deep = std::string(100000, '[') + std::string(100000, ']');
        const auto r = showFromJson(deep);
        REQUIRE_FALSE(r.ok());
    }
    SUBCASE("nesting bomb inside an object") {
        std::string deep = R"({"format":"stagesignal-show","schema":{"major":1,"minor":0},"metadata":)" + std::string(5000, '[') + std::string(5000, ']') + "}";
        CHECK_FALSE(showFromJson(deep).ok());
    }
    SUBCASE("not json / not a show") {
        CHECK_FALSE(showFromJson("garbage").ok());
        CHECK_FALSE(showFromJson("[]").ok());
        CHECK_FALSE(showFromJson("{}").ok());
        CHECK_FALSE(showFromJson(R"({"format":"other","schema":{"major":1,"minor":0}})").ok());
        CHECK_FALSE(showFromJson(R"({"format":"stagesignal-show"})").ok());
    }
    SUBCASE("too many cues") {
        auto j = fx::toJ(fx::sampleDoc());
        j["cues"] = json::array();
        for (int i = 0; i < 10001; ++i) j["cues"].push_back({{"id", i}, {"sceneId", 1}});
        const auto r = showFromJson(j.dump());
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().category == ErrorCategory::ResourceExhausted);
    }
    SUBCASE("exactly 10000 cues is accepted") {
        auto j = fx::toJ(fx::sampleDoc());
        j["cues"] = json::array();
        for (int i = 0; i < 10000; ++i) j["cues"].push_back({{"id", i}, {"sceneId", 1}});
        CHECK(showFromJson(j.dump()).ok());
    }
    SUBCASE("too many scenes / messages / midi / patches") {
        for (auto [key, count] : {std::pair<const char*, int>{"scenes", 1001}, {"messages", 257}, {"midi", 513}, {"patches", 257}}) {
            auto j = fx::toJ(fx::sampleDoc());
            j[key] = json::array();
            for (int i = 0; i < count; ++i) j[key].push_back({{"id", i}});
            const auto r = showFromJson(j.dump());
            INFO(key);
            REQUIRE_FALSE(r.ok());
            CHECK(r.error().category == ErrorCategory::ResourceExhausted);
        }
    }
    SUBCASE("type confusion and out-of-range numbers") {
        auto j = fx::toJ(fx::sampleDoc()); j["cues"][0]["id"] = "one"; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["cues"][0]["id"] = -1; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["cues"][0]["id"] = 5000000000ull; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["scenes"][0]["seed"] = "12x"; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["scenes"][0]["seed"] = "99999999999999999999999"; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["scenes"][0]["params"]["prismSegments"] = 40; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["scenes"][0]["params"]["brightness"] = 1e30; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["scenes"] = "none"; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["lighting"]["rateHz"] = 500; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["lighting"]["destination"] = "evil.example.com"; CHECK_FALSE(showFromJson(j.dump()).ok());
    }
    SUBCASE("dangling references") {
        auto j = fx::toJ(fx::sampleDoc()); j["cues"][0]["sceneId"] = 99; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["cues"][0]["mediaId"] = 99; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["cues"][0]["textId"] = 99; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["patches"][0]["profileId"] = "nope"; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["outputs"][1]["duplicateOf"] = 9; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["scenes"][1]["id"] = 1; CHECK_FALSE(showFromJson(j.dump()).ok());   // duplicate id
    }
    SUBCASE("patch must fit a universe") {
        auto j = fx::toJ(fx::sampleDoc()); j["patches"][0]["startAddress"] = 511; CHECK_FALSE(showFromJson(j.dump()).ok());
        j = fx::toJ(fx::sampleDoc()); j["patches"][0]["startAddress"] = 0; CHECK_FALSE(showFromJson(j.dump()).ok());
    }
}

TEST_CASE("REQ-S14: media paths must be safe relative bundle paths") {
    for (const char* p : {"../x.png", "/etc/passwd", "C:/x.png", "\\\\host\\s\\x.png", "http://evil.example/x.png", "media/../../x.png",
                          "data:image/png;base64,AA", "media\\x.png", "file:///x.png", ".hidden"}) {
        auto j = fx::toJ(fx::sampleDoc());
        j["media"][0]["relativePath"] = p;
        INFO(p);
        CHECK_FALSE(showFromJson(j.dump()).ok());
    }
    auto j = fx::toJ(fx::sampleDoc());
    j["media"][0]["relativePath"] = "";   // excluded media is allowed
    CHECK(showFromJson(j.dump()).ok());
    j["media"][0]["sha256"] = "XYZ";
    CHECK_FALSE(showFromJson(j.dump()).ok());
    j = fx::toJ(fx::sampleDoc()); j["media"][0]["format"] = "exe";
    CHECK_FALSE(showFromJson(j.dump()).ok());
}

TEST_CASE("REQ-S13: text messages are limited to 4096 code points via ss::utf8") {
    auto doc = fx::sampleDoc();
    std::string snow;
    for (int i = 0; i < 4096; ++i) snow += "\xE2\x98\x83";   // 3 bytes each: 12288 bytes but 4096 code points
    REQUIRE(utf8::countCodePoints(snow) == 4096);
    doc.messages[0].utf8 = snow;
    CHECK(showToJson(doc).ok());
    doc.messages[0].utf8 = snow + "x";
    CHECK_FALSE(showToJson(doc).ok());
    // via the parser too
    auto j = fx::toJ(fx::sampleDoc());
    j["messages"][0]["utf8"] = snow + "x";
    CHECK_FALSE(showFromJson(j.dump()).ok());
    // invalid UTF-8 in memory
    doc.messages[0].utf8 = "bad\xC0\xAF";
    CHECK_FALSE(showToJson(doc).ok());
    // control characters other than newline/tab
    doc.messages[0].utf8 = std::string("a\x01" "b");
    CHECK_FALSE(showToJson(doc).ok());
}

TEST_CASE("IF-11: portable export drops physical identifiers, local destinations and hardware hashes") {
    const auto doc = fx::sampleDoc();
    JsonWriteOptions local; local.mode = ExportMode::Local;
    JsonWriteOptions portable; portable.mode = ExportMode::Portable;
    const auto l = showToJson(doc, local).value();
    const auto p = showToJson(doc, portable).value();
    CHECK(has(l, "SERIAL0001"));
    CHECK(has(l, "192.168.1.50"));
    CHECK(has(l, "ab12cd34"));
    CHECK_FALSE(has(p, "SERIAL0001"));
    CHECK_FALSE(has(p, "VID_1234"));
    CHECK_FALSE(has(p, "192.168.1.50"));
    CHECK_FALSE(has(p, "ab12cd34"));
    CHECK(has(p, "Launch Pad"));            // non-secret matching hint stays
    CHECK(has(p, "27 inch wall display"));
    auto back = showFromJson(p);
    REQUIRE(back.ok());
    CHECK(back.value().doc.midi[0].deviceIdentity.empty());
    CHECK(back.value().doc.lighting.destination.empty());
}

TEST_CASE("REQ-B03: journal details are sanitised on write and secrets in fields are reported by path only") {
    auto doc = fx::sampleDoc();
    doc.journal.push_back({3000, 1, "cast", "start rtmps://live.example.com/app/abcd1234efgh5678ijkl key=SuperSecret99"});
    const auto text = showToJson(doc).value();
    CHECK_FALSE(has(text, "abcd1234efgh5678ijkl"));
    CHECK_FALSE(has(text, "SuperSecret99"));
    doc = fx::sampleDoc();
    doc.messages[0].utf8 = "wifi password: hunter2hunter2";
    const auto hits = scanForSecrets(doc);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0] == "messages[0].utf8");
    CHECK_FALSE(has(hits[0], "hunter2"));
}

TEST_CASE("REQ-S14: writer refuses documents that fail validation") {
    auto doc = fx::sampleDoc();
    doc.cues[0].sceneId = 42;
    CHECK_FALSE(showToJson(doc).ok());
    doc = fx::sampleDoc();
    doc.scenes[0].params.brightness = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(showToJson(doc).ok());
    doc = fx::sampleDoc();
    doc.schemaMajor = 5;
    CHECK(showToJson(doc).ok());   // writer stamps the current schema
    doc = fx::sampleDoc();
    doc.outputs.clear();
    CHECK_FALSE(showToJson(doc).ok());
}

TEST_CASE("REQ-S13: peekSchema reads the version without building a document") {
    const auto text = showToJson(fx::sampleDoc()).value();
    const auto v = peekSchema(text);
    REQUIRE(v.ok());
    CHECK(v.value().major == model::kSchemaMajor);
    CHECK_FALSE(peekSchema("nope").ok());
    CHECK_FALSE(peekSchema(R"({"schema":{"major":0,"minor":0}})").ok());
}
