#include "helpers.hpp"

using namespace testutil;

TEST_CASE("REQ-M01: note-on velocity 0 is normalised to note-off") {
    const std::uint8_t b[] = {0x92, 60, 0, 0x92, 61, 100};
    auto v = parsePacket(b, sizeof b);
    REQUIRE(v.size() == 2);
    CHECK(v[0].kind == MsgKind::NoteOff);
    CHECK(v[0].fromZeroVelocity);
    CHECK(v[0].channel == 2);
    CHECK(v[0].data1 == 60);
    CHECK(v[1].kind == MsgKind::NoteOn);
    CHECK(v[1].data2 == 100);
}

TEST_CASE("REQ-M01: parser handles note, CC, program change, running status and interleaved realtime") {
    Parser p;
    std::vector<Message> out;
    const std::uint8_t b[] = {0xB1, 7, 64, 8, 65, 0xF8, 9, 66, 0xC3, 12, 0x81, 40, 0};
    p.feed(b, sizeof b, 0, out);
    REQUIRE(out.size() == 6);
    CHECK(out[0].kind == MsgKind::ControlChange);
    CHECK(out[1].data1 == 8);                      // running status
    CHECK(out[2].kind == MsgKind::Clock);          // realtime in the middle of a message
    CHECK(out[3].data1 == 9);                      // ... did not break the CC in flight
    CHECK(out[3].data2 == 66);
    CHECK(out[4].kind == MsgKind::ProgramChange);
    CHECK(out[4].channel == 3);
    CHECK(out[5].kind == MsgKind::NoteOff);
}

TEST_CASE("REQ-M02: clock, start, continue and stop bytes parse; active sensing is ignored") {
    const std::uint8_t b[] = {0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF};
    Parser p;
    std::vector<Message> out;
    p.feed(b, sizeof b, 0, out);
    REQUIRE(out.size() == 4);
    CHECK(out[0].kind == MsgKind::Clock);
    CHECK(out[1].kind == MsgKind::Start);
    CHECK(out[2].kind == MsgKind::Continue);
    CHECK(out[3].kind == MsgKind::Stop);
    CHECK(p.stats().ignoredRealtime == 2);
}

TEST_CASE("REQ-M01: unsupported and malformed bytes are counted, never mis-parsed") {
    Parser p;
    std::vector<Message> out;
    const std::uint8_t b[] = {0x10, 0x20,            // data bytes with no status
                              0xE0, 1, 2,            // pitch bend: unsupported
                              0xF2, 5, 6,            // song position: swallowed
                              0x90, 60, 90};
    p.feed(b, sizeof b, 0, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == MsgKind::NoteOn);
    CHECK(p.stats().malformedBytes == 2);
    CHECK(p.stats().unsupportedMessages >= 2);
}

TEST_CASE("REQ-M01: SysEx is disabled by default and dropped") {
    Parser p;
    std::vector<Message> out;
    const std::uint8_t b[] = {0xF0, 0x7E, 1, 2, 3, 0xF7, 0x90, 60, 100};
    p.feed(b, sizeof b, 0, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == MsgKind::NoteOn);
    CHECK(p.stats().sysexBlocked == 1);
}

TEST_CASE("REQ-M01: SysEx when enabled is bounded to 1024 bytes") {
    Parser p(true);
    std::vector<Message> out;
    std::vector<std::uint8_t> ok{0xF0};
    ok.resize(1023, 0x11);
    ok.push_back(0xF7);                              // exactly 1024 bytes
    p.feed(ok.data(), ok.size(), 0, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].sysex.size() == 1024);

    std::vector<std::uint8_t> big{0xF0};
    big.resize(1024, 0x22);
    big.push_back(0xF7);                             // 1025 bytes: refused
    out.clear();
    p.feed(big.data(), big.size(), 0, out);
    CHECK(out.empty());
    CHECK(p.stats().sysexOversize == 1);

    // a huge frame never grows the buffer without bound
    std::vector<std::uint8_t> huge(200000, 0x33);
    huge.front() = 0xF0; huge.back() = 0xF7;
    p.feed(huge.data(), huge.size(), 0, out);
    CHECK(out.empty());
    CHECK(p.stats().sysexOversize == 2);
}

TEST_CASE("REQ-M01: a status byte aborts an unfinished SysEx frame") {
    Parser p(true);
    std::vector<Message> out;
    const std::uint8_t b[] = {0xF0, 1, 2, 0x90, 60, 100};
    p.feed(b, sizeof b, 0, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == MsgKind::NoteOn);
    CHECK(p.stats().sysexAborted == 1);
}

TEST_CASE("REQ-M02: encode round-trips through the parser") {
    const Message msgs[] = {makeNoteOn(3, 64, 100), makeNoteOff(3, 64), makeCC(15, 7, 127), makeProgramChange(1, 99),
                            makeRealtime(MsgKind::Clock), makeRealtime(MsgKind::Stop)};
    for (const auto& m : msgs) {
        std::uint8_t b[3];
        const std::size_t n = encode(m, b);
        REQUIRE(n > 0);
        auto v = parsePacket(b, n);
        REQUIRE(v.size() == 1);
        CHECK(v[0] == m);
    }
}
