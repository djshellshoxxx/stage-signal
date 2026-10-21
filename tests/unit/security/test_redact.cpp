#include "doctest/doctest.h"
#include <string>
#include "ss/base/rng.hpp"
#include "ss/security/redact.hpp"
#include "ss/security/secret_store.hpp"

using namespace ss::security;

static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

TEST_CASE("REQ-B03: URL userinfo, query and fragment are stripped") {
    const auto r = redactText("connect https://bob:hunter2@example.com/path/x?token=abc123&u=1#frag now");
    CHECK_FALSE(has(r, "hunter2"));
    CHECK_FALSE(has(r, "bob:"));
    CHECK_FALSE(has(r, "abc123"));
    CHECK_FALSE(has(r, "frag"));
    CHECK(has(r, "https://example.com/path/x"));
    CHECK(has(r, " now"));
}

TEST_CASE("REQ-B03: stream key path segments are stripped for rtmp, rtmps and srt") {
    CHECK(redactText("rtmps://live.example.com:443/live2/abcd-1234-efgh-5678") == "rtmps://live.example.com:443/live2/<redacted>");
    CHECK(redactText("rtmp://a.b/app/key") == "rtmp://a.b/app/<redacted>");
    CHECK(redactText("srt://h:9000/x/y?streamid=secretvalue&passphrase=p4ssw0rdp4ss") == "srt://h:9000/x/<redacted>");
    // key-looking segment on a non-stream scheme
    CHECK_FALSE(has(redactText("https://x.example/hook/AbCdEf0123456789XyZ"), "AbCdEf0123456789XyZ"));
    // ordinary readable path is left alone
    CHECK(redactText("https://x.example/docs/getting-started") == "https://x.example/docs/getting-started");
}

TEST_CASE("REQ-B03: filesystem usernames are replaced") {
    CHECK(redactText("open /home/sheldon/shows/a.json failed") == "open /home/<user>/shows/a.json failed");
    CHECK(redactText("open /Users/Sheldon Davidson/x") == "open /Users/<user> Davidson/x");   // first token only; space ends the name
    CHECK(redactText("C:\\Users\\sdavidson\\Documents\\a.json") == "C:\\Users\\<user>\\Documents\\a.json");
    CHECK(redactText("c:/users/sdavidson/x") == "c:/users/<user>/x");
    CHECK(redactText("C:\\\\Users\\\\sdavidson\\\\x") == "C:\\\\Users\\\\<user>\\\\x");
    CHECK(redactText("apps/home/index") == "apps/home/index");   // not a user path
}

TEST_CASE("REQ-B03: secret-like option values are redacted in many spellings") {
    struct Case { const char* in; const char* secret; };
    const Case cases[] = {
        {"key=SuperSecret99", "SuperSecret99"},
        {"opts: stream_key=live_abc123 rate=5", "live_abc123"},
        {"token=tok_12345678", "tok_12345678"},
        {"passphrase=correct-horse-battery", "correct-horse-battery"},
        {"password: hunter2!", "hunter2!"},
        {"{\"password\": \"s3cret value\", \"x\": 1}", "s3cret value"},
        {"ffmpeg -srt_passphrase mypassphrase123 -i in", "mypassphrase123"},
        {"--api-key abcdef123456 --verbose", "abcdef123456"},
        {"Authorization: Bearer eyJhbGciOi.payload.sig", "eyJhbGciOi"},
        {"got Bearer abcdefghijkl1234 back", "abcdefghijkl1234"},
        {"accessToken=zzzzzzzz&next=1", "zzzzzzzz"},
        {"PASSWD='quoted pass'", "quoted pass"},
    };
    for (const auto& c : cases) {
        const auto r = redact(c.in);
        INFO(c.in);
        CHECK_FALSE(has(r.text, c.secret));
        CHECK(r.replacements >= 1);
    }
}

TEST_CASE("REQ-B03: ordinary text and hashes are not mangled") {
    const std::string benign[] = {
        "scene changed to Neon Grid at bar 33",
        "bpm=128.0 tonic=A minor",
        "monkey=banana keyboard=us",
        "sha256 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
        "Key: C minor",
        "https://example.com/users/42",
    };
    for (const auto& s : benign) {
        INFO(s);
        CHECK(redactText(s) == s);
    }
}

TEST_CASE("REQ-B03: redaction is idempotent and bounded on random hostile input") {
    ss::Rng rng(42);
    const char alphabet[] = "abcXYZ019=:/?&#@\\\"' -_.<>[]{}\n\t%";
    for (int n = 0; n < 400; ++n) {
        std::string s;
        const auto len = rng.below(120);
        for (std::uint32_t i = 0; i < len; ++i) s += alphabet[rng.below(sizeof alphabet - 1)];
        if (n % 3 == 0) s = "rtmp://u:p@h/app/" + s + " key=" + s + " /home/" + s;
        const auto once = redactText(s);
        CHECK(redactText(once) == once);
        CHECK(once.size() <= s.size() * 3 + 64);
    }
}

TEST_CASE("REQ-B03: Redactor scrubs literal secret values from a store") {
    InMemorySecretStore store;
    CHECK(store.put("secret:youtube-key", "xK9-plain-literal-77").ok());
    const auto red = makeRedactorFor(store);
    const auto r = red.apply("error talking to host with xK9-plain-literal-77 in the middle");
    CHECK_FALSE(has(r.text, "xK9-plain-literal-77"));
    CHECK(r.replacements == 1);
    Redactor tiny;
    tiny.addLiteral("ab");
    CHECK(tiny.literalCount() == 0);
}

TEST_CASE("REQ-B03: sanitizeForLog strips control characters, bad UTF-8 and truncates on a boundary") {
    CHECK(sanitizeForLog("a\nb\tc\x01" "d") == "a b c d");
    CHECK(sanitizeForLog("bad\xff\xfe" "x") == "bad??x");
    const std::string snow = "\xE2\x98\x83\xE2\x98\x83\xE2\x98\x83";   // three 3-byte code points
    CHECK(truncateUtf8(snow, 7) == "\xE2\x98\x83\xE2\x98\x83");
    CHECK(sanitizeForLog(snow, 4) == "\xE2\x98\x83");
    CHECK(sanitizeForLog("password=hunter2 /home/bob/x", 256) == "password=<redacted> /home/<user>/x");
}
