#include <algorithm>
#include <cmath>
#include <limits>
#include "helpers.hpp"
#include "ss/base/rng.hpp"

using namespace testutil;

namespace {
constexpr Ns ms(int v) { return static_cast<Ns>(v) * kNsPerMs; }

// Worker loop model: call due() every `step` ns from `from` (exclusive) to `to` (inclusive).
void runWorker(OutputScheduler& o, Ns from, Ns to, std::vector<OutPacket>& got, Ns step = kNsPerMs) {
    for (Ns t = from + step; t <= to; t += step) o.due(t, got);
}

std::vector<Ns> intervalErrorsMs(const std::vector<Ns>& t, double idealMs) {
    std::vector<double> e;
    for (std::size_t i = 1; i < t.size(); ++i) e.push_back(std::fabs(nsToMs(t[i] - t[i - 1]) - idealMs));
    std::sort(e.begin(), e.end());
    return {static_cast<Ns>(e[e.size() / 2] * 1e6), static_cast<Ns>(e[static_cast<std::size_t>(e.size() * 0.95)] * 1e6)};
}
}  // namespace

TEST_CASE("REQ-M02: notes and CC are delivered on the selected port in deadline order") {
    OutputScheduler o;
    o.openPort(1, 0);
    o.openPort(2, 0);
    REQUIRE(o.send(1, makeNoteOn(0, 60, 100), ms(20), ActionSource::Show, 0).ok());
    REQUIRE(o.send(1, makeCC(0, 7, 90), ms(10), ActionSource::Show, 0).ok());
    REQUIRE(o.send(2, makeNoteOn(1, 62, 80), ms(15), ActionSource::Show, 0).ok());
    REQUIRE(o.send(1, makeNoteOff(0, 60), ms(30), ActionSource::Show, 0).ok());
    std::vector<OutPacket> got;
    o.due(ms(9), got);
    CHECK(got.empty());                                    // nothing before its deadline
    o.due(ms(40), got);
    REQUIRE(got.size() == 4);
    CHECK(got[0].midi.portId == 1);  CHECK(got[0].msg.kind == MsgKind::ControlChange);
    CHECK(got[1].midi.portId == 2);  CHECK(got[1].msg.channel == 1);
    CHECK(got[2].msg.kind == MsgKind::NoteOn);
    CHECK(got[3].msg.kind == MsgKind::NoteOff);
    CHECK(got[0].midi.bytes[0] == 0xB0);
    CHECK(got[0].midi.bytes[1] == 7);
    CHECK(got[0].midi.bytes[2] == 90);
    CHECK(got[0].midi.size == 3);
    CHECK(got[0].midi.generation == o.portGeneration(1));
}

TEST_CASE("REQ-M02: out-of-range data and unknown or closed ports are rejected before queueing") {
    OutputScheduler o;
    CHECK_FALSE(o.send(5, makeNoteOn(0, 60, 100), 0, ActionSource::Show, 0).ok());     // never opened
    o.openPort(1, 0);
    Message bad = makeNoteOn(16, 60, 100);
    CHECK_FALSE(o.send(1, bad, 0, ActionSource::Show, 0).ok());
    bad = makeNoteOn(0, 128, 100);
    CHECK_FALSE(o.send(1, bad, 0, ActionSource::Show, 0).ok());
    CHECK(o.pending() == 0);
    o.portLost(1);
    auto s = o.send(1, makeNoteOn(0, 60, 100), 0, ActionSource::Show, 0);
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().category == ErrorCategory::DeviceLost);
}

TEST_CASE("REQ-M02: MIDI clock is opt-in and runs at 24 pulses per quarter note") {
    OutputScheduler o;
    FixedTempo tempo(120.0);
    o.setTempoSource(&tempo);
    o.openPort(1, 0);
    std::vector<OutPacket> got;
    o.due(kNsPerSec, got);
    CHECK(got.empty());                                      // default: no clock
    CHECK_FALSE(o.sendTransport(MsgKind::Start, kNsPerSec).ok());   // transport also needs the opt-in
    ClockConfig c; c.enabled = true; c.portId = 1;
    REQUIRE(o.configureClock(c, kNsPerSec).ok());
    runWorker(o, kNsPerSec, 2 * kNsPerSec, got);             // one second at 120 BPM = 2 beats = 48 pulses
    int clocks = 0;
    for (auto& p : got) clocks += p.msg.kind == MsgKind::Clock;
    CHECK(clocks == 48);
    CHECK(got[0].midi.bytes[0] == 0xF8);
}

TEST_CASE("REQ-M02: clock timing over 10 min at 120 BPM with a fake clock (median <=1 ms, p95 <=3 ms, no missing pulses)") {
    FakeClock clock(0);
    OutputScheduler o;
    FixedTempo tempo(120.0);
    o.setTempoSource(&tempo);
    o.openPort(1, 0);
    ClockConfig c; c.enabled = true; c.portId = 1;
    o.configureClock(c, clock.nowMonotonic());

    // Worker model: sleep until nextWake(), then the OS wakes it up to 2 ms late (uniform, seeded).
    Rng rng(7);
    std::vector<Ns> sentAt, deadlines;
    std::vector<OutPacket> got;
    const Ns end = 600 * kNsPerSec;
    while (clock.nowMonotonic() <= end) {
        const Ns wake = o.nextWake(clock.nowMonotonic());
        REQUIRE(wake < std::numeric_limits<Ns>::max());
        const Ns late = static_cast<Ns>(rng.below(2000)) * 1000;       // 0..2 ms in ns
        clock.set(std::max(clock.nowMonotonic(), wake + late));
        got.clear();
        o.due(clock.nowMonotonic(), got);
        for (auto& p : got) { sentAt.push_back(clock.nowMonotonic()); deadlines.push_back(p.midi.deadline); }
    }
    // 10 min * 120 BPM * 24 = 28800 pulses; the one due exactly at 600 s may or may not be inside the loop bound
    CHECK(sentAt.size() >= 28799);
    CHECK(sentAt.size() <= 28800);
    // deadlines sit on the exact grid: no accumulated drift and no missing pulse
    const double idealNs = 2.5e9 / 120.0;
    double maxDev = 0;
    for (std::size_t i = 0; i < deadlines.size(); ++i)
        maxDev = std::max(maxDev, std::fabs(static_cast<double>(deadlines[i]) - (static_cast<double>(i + 1) * idealNs + 0.0)));
    CHECK(maxDev <= 1.0);                                   // within 1 ns of the ideal grid (rounding)
    // what a receiver would observe with the stated wake-up jitter
    auto q = intervalErrorsMs(sentAt, idealNs / 1e6);
    CHECK(nsToMs(q[0]) <= 1.0);
    CHECK(nsToMs(q[1]) <= 3.0);
    CHECK(o.stats().clockDiscarded == 0);
    CHECK(o.stats().maxLateNs < 2 * kNsPerMs);
}

TEST_CASE("REQ-M02: tempo changes follow the tempo source without a pulse burst or gap") {
    FakeClock clock(0);
    OutputScheduler o;
    FixedTempo tempo(120.0);
    o.setTempoSource(&tempo);
    o.openPort(1, 0);
    ClockConfig c; c.enabled = true; c.portId = 1;
    o.configureClock(c, 0);
    std::vector<OutPacket> got;
    std::vector<Ns> dl;
    for (Ns t = 0; t < 10 * kNsPerSec; t += kNsPerMs) {
        if (t == 5 * kNsPerSec) tempo.set(150.0);
        got.clear();
        o.due(t, got);
        for (auto& p : got) dl.push_back(p.midi.deadline);
    }
    REQUIRE(dl.size() > 100);
    for (std::size_t i = 1; i < dl.size(); ++i) {
        const double step = nsToMs(dl[i] - dl[i - 1]);
        if (dl[i] < 4970 * kNsPerMs) CHECK(std::fabs(step - 20.8333) < 0.01);
        else if (dl[i - 1] >= 4990 * kNsPerMs) CHECK(std::fabs(step - 16.6667) < 0.01);
        CHECK(step < 21.0);                                  // never a gap
        CHECK(step > 16.0);                                  // never a burst
    }
}

TEST_CASE("REQ-M02: an unusable tempo holds the clock instead of guessing, then resumes cleanly") {
    OutputScheduler o;
    FixedTempo tempo(0.0);                                   // e.g. no estimate yet
    o.setTempoSource(&tempo);
    o.openPort(1, 0);
    ClockConfig c; c.enabled = true; c.portId = 1;
    o.configureClock(c, 0);
    std::vector<OutPacket> got;
    runWorker(o, 0, 5 * kNsPerSec, got);
    CHECK(got.empty());
    CHECK(o.stats().tempoInvalid >= 1);
    tempo.set(120.0);
    runWorker(o, 5 * kNsPerSec, 6 * kNsPerSec, got);         // re-anchors at the first call after the tempo appears
    CHECK(got.size() >= 46);
    CHECK(got.size() <= 48);
    tempo.set(1000.0);                                       // absurd: also held
    got.clear();
    runWorker(o, 6 * kNsPerSec, 8 * kNsPerSec, got);
    CHECK(got.empty());
}

TEST_CASE("REQ-M02: a stalled worker discards late clock pulses instead of bursting them") {
    OutputScheduler o;
    FixedTempo tempo(120.0);
    o.setTempoSource(&tempo);
    o.openPort(1, 0);
    ClockConfig c; c.enabled = true; c.portId = 1;
    o.configureClock(c, 0);
    std::vector<OutPacket> got;
    runWorker(o, 0, ms(100), got);
    const std::size_t before = got.size();
    got.clear();
    o.due(ms(2000), got);                                    // worker stalled for 1.9 s
    CHECK(got.size() < 5);                                   // only pulses within the 50 ms staleness window
    CHECK(o.stats().clockDiscarded > 50);
    for (auto& p : got) CHECK(p.lateNs <= kClockStaleNs);
    CHECK(before >= 4);
}

TEST_CASE("REQ-M02: Start, Stop and Continue are separately mapped and clock-gated") {
    OutputScheduler o;
    FixedTempo tempo(120.0);
    o.setTempoSource(&tempo);
    o.openPort(1, 0);
    ClockConfig c; c.enabled = true; c.portId = 1;
    o.configureClock(c, 0);
    TransportMap tm;
    tm.start = ActionId::Resume;                             // only Start and Stop are mapped
    tm.stop = ActionId::Pause;
    o.setTransportMap(tm);
    CHECK_FALSE(o.onShowAction(ActionId::NextScene, ms(1)));  // unrelated
    CHECK(o.onShowAction(ActionId::Resume, ms(1)));
    CHECK(o.onShowAction(ActionId::Pause, ms(2)));
    CHECK_FALSE(o.onShowAction(ActionId::PrevScene, ms(3)));
    std::vector<OutPacket> got;
    o.due(ms(5), got);
    std::vector<MsgKind> kinds;
    for (auto& p : got) if (p.msg.kind != MsgKind::Clock) kinds.push_back(p.msg.kind);
    REQUIRE(kinds.size() == 2);
    CHECK(kinds[0] == MsgKind::Start);
    CHECK(kinds[1] == MsgKind::Stop);
    // Continue was never mapped
    CHECK(o.sendTransport(MsgKind::Continue, ms(6)).ok());   // explicit manual send still works while clock is on
    got.clear();
    o.due(ms(7), got);
    REQUIRE(!got.empty());
    CHECK(got.back().msg.kind == MsgKind::Continue);
    // with the clock turned off, transport is refused
    c.enabled = false;
    o.configureClock(c, ms(8));
    CHECK_FALSE(o.sendTransport(MsgKind::Stop, ms(9)).ok());
}

TEST_CASE("REQ-M02: Stop outranks other messages due at the same instant") {
    OutputScheduler o;
    FixedTempo tempo(120.0);
    o.setTempoSource(&tempo);
    o.openPort(1, 0);
    ClockConfig c; c.enabled = true; c.portId = 1;
    o.configureClock(c, 0);
    o.send(1, makeCC(0, 1, 5), ms(10), ActionSource::Show, 0);
    o.send(1, makeNoteOn(0, 60, 100), ms(10), ActionSource::Show, 0);
    REQUIRE(o.sendTransport(MsgKind::Stop, ms(10)).ok());   // same instant as the note and CC above
    std::vector<OutPacket> got;
    o.due(ms(10), got);
    REQUIRE(got.size() >= 3);
    CHECK(got.front().msg.kind == MsgKind::Stop);
    CHECK(got[1].msg.kind != MsgKind::NoteOn);               // clock/CC order is secondary, Stop is first
}

TEST_CASE("REQ-M02: StageSignal tracks only notes it owns and releases exactly those (no All Notes Off)") {
    OutputScheduler o;
    o.openPort(1, 0);
    o.openPort(2, 0);
    o.send(1, makeNoteOn(0, 60, 100), 0, ActionSource::Show, 0);
    o.send(1, makeNoteOn(0, 64, 100), 0, ActionSource::Show, 0);
    o.send(1, makeNoteOn(3, 67, 100), 0, ActionSource::Show, 0);
    o.send(2, makeNoteOn(0, 48, 100), 0, ActionSource::Show, 0);
    std::vector<OutPacket> got;
    o.due(ms(1), got);
    CHECK(o.ownedNoteCount(1) == 3);
    o.send(1, makeNoteOff(0, 64), ms(2), ActionSource::Show, ms(1));
    o.due(ms(2), got);
    CHECK(o.ownedNoteCount(1) == 2);
    // a queued note-on must not slip out after cleanup
    o.send(1, makeNoteOn(0, 72, 100), ms(500), ActionSource::Show, ms(2));
    got.clear();
    CHECK(o.releaseOwned(1, ms(3)) == 2);
    o.due(ms(1000), got);
    REQUIRE(got.size() == 2);
    for (auto& p : got) {
        CHECK(p.msg.kind == MsgKind::NoteOff);
        CHECK(p.midi.portId == 1);
        CHECK(p.midi.bytes[1] != 0x7B);
    }
    CHECK(o.ownedNoteCount(1) == 0);
    CHECK(o.ownedNoteCount(2) == 1);                         // the other device's note is untouched
    // never an unsolicited All Notes Off (CC 123) anywhere
    got.clear();
    o.releaseAllOwned(ms(1001));
    o.due(ms(1002), got);
    for (auto& p : got) CHECK_FALSE((p.midi.bytes[0] & 0xF0) == 0xB0);
    CHECK(o.ownedNoteCount(2) == 0);
}

TEST_CASE("REQ-M04: output port loss drops queued items; reconnect releases only notes we owned") {
    OutputScheduler o;
    const Generation g1 = o.openPort(1, 0);
    o.send(1, makeNoteOn(0, 60, 100), 0, ActionSource::Show, 0);
    std::vector<OutPacket> got;
    o.due(ms(1), got);
    o.send(1, makeCC(0, 7, 1), ms(100), ActionSource::Show, ms(1));
    o.send(1, makeNoteOn(0, 61, 100), ms(100), ActionSource::Show, ms(1));
    o.portLost(1);
    CHECK(o.pending() == 0);
    got.clear();
    o.due(ms(200), got);
    CHECK(got.empty());                                      // nothing stale leaks out
    const Generation g2 = o.openPort(1, ms(300));
    CHECK(g2 != g1);
    o.due(ms(301), got);
    REQUIRE(got.size() == 1);                                // note 60 was ours, note 61 never left the queue
    CHECK(got[0].msg.kind == MsgKind::NoteOff);
    CHECK(got[0].msg.data1 == 60);
}

TEST_CASE("REQ-M02: late note-on and CC are discarded as stale, note-off never") {
    OutputScheduler o;
    o.openPort(1, 0);
    o.send(1, makeNoteOn(0, 60, 100), ms(10), ActionSource::Show, 0);
    o.send(1, makeCC(0, 7, 1), ms(10), ActionSource::Show, 0);
    o.send(1, makeNoteOff(0, 61), ms(10), ActionSource::Show, 0);
    std::vector<OutPacket> got;
    o.due(ms(10) + kNoteOnStaleNs + 1, got);
    REQUIRE(got.size() == 1);
    CHECK(got[0].msg.kind == MsgKind::NoteOff);
    CHECK(o.stats().staleDiscarded == 2);
}

TEST_CASE("REQ-M02: already-due CC values coalesce while future scheduled ones stay distinct") {
    OutputScheduler o;
    o.openPort(1, 0);
    o.send(1, makeCC(0, 7, 10), 0, ActionSource::Show, ms(1));
    o.send(1, makeCC(0, 7, 20), 0, ActionSource::Show, ms(2));
    o.send(1, makeCC(0, 7, 30), 0, ActionSource::Show, ms(3));
    CHECK(o.pending() == 1);
    o.send(1, makeCC(0, 8, 1), ms(100), ActionSource::Show, ms(3));
    o.send(1, makeCC(0, 8, 2), ms(200), ActionSource::Show, ms(3));
    CHECK(o.pending() == 3);
    std::vector<OutPacket> got;
    o.due(ms(200), got);
    REQUIRE(got.size() == 3);
    CHECK(got[0].msg.data2 == 30);
}

TEST_CASE("REQ-M02: output queue is bounded; must-deliver items still fit") {
    OutputScheduler o;
    o.openPort(1, 0);
    int refused = 0;
    for (int i = 0; i < 700; ++i) {
        auto s = o.send(1, makeNoteOn(0, static_cast<std::uint8_t>(i % 100), 100), ms(1000) + i, ActionSource::Show, 0);
        if (!s.ok()) ++refused;
    }
    CHECK(refused == 700 - static_cast<int>(kOutputQueueCapacity));
    CHECK(o.stats().queueRejected == static_cast<std::uint64_t>(refused));
    CHECK(o.send(1, makeNoteOff(0, 5), ms(1000), ActionSource::Show, 0).ok());     // note-offs always fit
}

TEST_CASE("REQ-M02: SysEx is disabled by default and bounded to 1024 bytes when enabled") {
    OutputScheduler o;
    o.openPort(1, 0);
    std::vector<std::uint8_t> frame{0xF0, 0x7D, 1, 2, 3, 0xF7};
    auto s = o.sendSysex(1, frame, 0, ActionSource::Ui, 0);
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().category == ErrorCategory::PermissionDenied);
    o.setSysexEnabled(true);
    CHECK(o.sendSysex(1, frame, 0, ActionSource::Ui, 0).ok());
    std::vector<std::uint8_t> big{0xF0};
    big.resize(1023, 0x10);
    big.push_back(0xF7);
    CHECK(o.sendSysex(1, big, 0, ActionSource::Ui, 0).ok());                     // 1024 exactly
    big.insert(big.begin() + 1, 0x10);
    CHECK_FALSE(o.sendSysex(1, big, 0, ActionSource::Ui, 0).ok());                // 1025
    CHECK_FALSE(o.sendSysex(1, {0x01, 0x02}, 0, ActionSource::Ui, 0).ok());       // not a frame
    CHECK_FALSE(o.sendSysex(1, {0xF0, 0x90, 0xF7}, 0, ActionSource::Ui, 0).ok()); // 8-bit payload byte
    std::vector<OutPacket> got;
    o.due(ms(1), got);
    CHECK(got.size() == 2);
    CHECK(o.stats().sysexSent == 2);
}

TEST_CASE("REQ-M02: engine SysEx switch applies to input and output") {
    Rig r;
    CHECK(r.e.scheduler().sysexEnabled() == false);
    r.e.setSysexEnabled(true);
    CHECK(r.e.scheduler().sysexEnabled());
    const std::uint8_t b[] = {0xF0, 1, 2, 0xF7};
    r.e.onBytes(1, r.g, b, sizeof b, 0);
    CHECK(r.e.counters(1)->sysexRx == 1);
}
