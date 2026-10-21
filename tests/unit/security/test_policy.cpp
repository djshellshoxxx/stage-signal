#include "doctest/doctest.h"
#include <string>
#include "ss/security/endpoint.hpp"
#include "ss/security/secret_store.hpp"
#include "ss/security/sockets.hpp"
#include "ss/security/worker_limits.hpp"

using namespace ss::security;

TEST_CASE("REQ-S14: local content may never use a network protocol") {
    for (const char* s : {"http", "https", "ftp", "rtmp", "rtmps", "srt", "concat", "subfile", "data", "file", "tcp", "udp"})
        CHECK_FALSE(isProtocolAllowed(Context::LocalContent, s));
    CHECK_FALSE(parseEndpoint("rtmps://h/app", Context::LocalContent).ok());
}

TEST_CASE("REQ-S14: media references cannot smuggle a URL") {
    for (const char* s : {"http://x/a.png", "//host/share/a.png", "\\\\host\\share", "data:image/png;base64,AAAA",
                          "concat:a|b", "file:///etc/passwd", "rtmp://h/x", "subfile,,start,0:a"})
        CHECK(looksLikeUrlOrUri(s));
    CHECK_FALSE(looksLikeUrlOrUri("media/0123abcd.png"));
    CHECK_FALSE(looksLikeUrlOrUri("c.png"));
}

TEST_CASE("REQ-S14: stream endpoint allowlist") {
    auto ok = parseEndpoint("RTMPS://Live.Example.com/live2", Context::Stream);
    REQUIRE(ok.ok());
    CHECK(ok.value().label() == "rtmps://live.example.com:443/live2");
    CHECK(parseEndpoint("srt://203.0.113.9:9000", Context::Stream).ok());
    CHECK(parseEndpoint("rtmps://[2001:db8::1]:443/app", Context::Stream).ok());

    const char* bad[] = {
        "http://x/", "ftp://x/", "rtmp://x/app", "file:///x", "rtmps://user:pw@x/app", "rtmps://x/app?key=1",
        "rtmps://x/app#f", "rtmps://x/app/streamkey123", "srt://x", "srt://x:9000/path", "rtmps://x:0/app", "rtmps://x:70000/app",
        "rtmps://x:abc/app", "rtmps://-bad.example/app", "rtmps://ex ample/app", "rtmps:///app", "", "rtmps://x/ap p",
    };
    for (const char* b : bad) { INFO(b); CHECK_FALSE(parseEndpoint(b, Context::Stream).ok()); }

    EndpointPolicy plain; plain.allowPlainRtmp = true;
    CHECK(parseEndpoint("rtmp://x/app", Context::Stream, plain).ok());
    EndpointPolicy noLoop; noLoop.allowLoopback = false;
    CHECK_FALSE(parseEndpoint("srt://127.0.0.1:9000", Context::Stream, noLoop).ok());
    CHECK(parseEndpoint("srt://127.0.0.1:9000", Context::Stream).ok());
    CHECK(enabledProtocolList().size() == 3);
    CHECK(enabledProtocolList(plain).size() == 4);
}

TEST_CASE("REQ-S14: lighting destination must be a plain IPv4 unicast address") {
    CHECK(validateLightingDestination("192.168.1.50").ok());
    for (const char* b : {"", "0.0.0.0", "255.255.255.255", "1.2.3", "1.2.3.4.5", "256.1.1.1", "01.2.3.4", "a.b.c.d", "1.2.3.4 ", "example.com", "::1"})
        CHECK_FALSE(validateLightingDestination(b).ok());
}

TEST_CASE("REQ-B03: secret references and the in-memory store") {
    CHECK(isValidSecretRef("secret:youtube-main"));
    for (const char* b : {"", "secret:", "secret:UPPER", "secret:a b", "key:abc", "secret:../x", "secret:a/b"}) CHECK_FALSE(isValidSecretRef(b));
    CHECK(makeSecretRef("My Twitch Key!") == "secret:my-twitch-key-");
    CHECK(isValidSecretRef(makeSecretRef("")));

    InMemorySecretStore s;
    CHECK_FALSE(s.put("bad ref", "v").ok());
    CHECK_FALSE(s.put("secret:a", "").ok());
    CHECK_FALSE(s.put("secret:a", std::string(kMaxSecretBytes + 1, 'x')).ok());
    CHECK(s.put("secret:a", "one").ok());
    CHECK(s.put("secret:a", "two").ok());
    CHECK(s.get("secret:a").value() == "two");
    CHECK(s.exists("secret:a"));
    const auto missing = s.get("secret:nope");
    REQUIRE_FALSE(missing.ok());
    CHECK(s.list().size() == 1);
    CHECK(s.remove("secret:a").ok());
    CHECK(s.remove("secret:a").ok());   // idempotent
    CHECK_FALSE(s.exists("secret:a"));
}

TEST_CASE("REQ-X04: default socket declaration has no listeners and nothing enabled by default") {
    const auto decl = defaultSocketDeclaration();
    CHECK_FALSE(decl.empty());
    for (const auto& d : decl) {
        INFO(d.id);
        CHECK(d.direction == SocketDirection::Outbound);
        CHECK_FALSE(d.enabledByDefault);
        CHECK(d.scope == BindScope::None);
    }
}

TEST_CASE("REQ-X04: socket audit flags listeners") {
    CHECK(auditSockets({}).empty());
    CHECK(auditSockets({{"udp", 50000, false, BindScope::None}}).empty());   // outbound/ephemeral, not listening
    CHECK(auditSockets({{"tcp", 8080, true, BindScope::Loopback}}).size() == 1);
    CHECK(auditSockets({{"tcp", 8080, true, BindScope::Loopback}}, false).empty());
    CHECK(auditSockets({{"tcp", 80, true, BindScope::AnyInterface}}, false).size() == 1);
}

TEST_CASE("REQ-S14: worker limits defaults validate and unenforced limits are reported") {
    for (auto c : {WorkerClass::MediaDecoder, WorkerClass::Encoder, WorkerClass::ArchiveImporter}) {
        const auto l = defaultLimits(c);
        CHECK(validate(l).ok());
        CHECK_FALSE(l.allowNetwork);
        CHECK(l.reducedPrivileges);
        CHECK(l.privateTempDirOnly);
    }
    WorkerLimits z = defaultLimits(WorkerClass::MediaDecoder);
    z.maxWallMs = 0;
    CHECK_FALSE(validate(z).ok());
    z = defaultLimits(WorkerClass::MediaDecoder); z.maxMemoryBytes = 1;
    CHECK_FALSE(validate(z).ok());
    const auto none = unenforcedLimits(defaultLimits(WorkerClass::MediaDecoder), EnforcementReport{});
    CHECK(none.size() >= 6);
    EnforcementReport all; all.memory = all.wallTime = all.cpuTime = all.processCount = all.openFiles = true;
    all.networkBlocked = all.privateTemp = all.privilegeReduced = true;
    CHECK(unenforcedLimits(defaultLimits(WorkerClass::MediaDecoder), all).empty());
}
