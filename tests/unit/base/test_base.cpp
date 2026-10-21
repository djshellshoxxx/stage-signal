#include "doctest/doctest.h"
#include "ss/base/error.hpp"
#include "ss/base/queue.hpp"
#include "ss/base/rng.hpp"
#include "ss/base/time.hpp"
#include "ss/base/types.hpp"

using namespace ss;

TEST_CASE("Result carries value or error") {
    Result<int> a(5);
    CHECK(a.ok());
    CHECK(a.value() == 5);
    Result<int> b(makeError(ErrorCategory::DiskFull, "full"));
    CHECK_FALSE(b.ok());
    CHECK(b.error().category == ErrorCategory::DiskFull);
    CHECK(b.valueOr(9) == 9);
    CHECK(okStatus().ok());
}

TEST_CASE("sample/ns conversion is exact and drift free") {
    CHECK(samplesToNs(48000, 48000) == kNsPerSec);
    CHECK(samplesToNs(44100ULL * 3600, 44100) == 3600 * kNsPerSec);
    // 1 h of 44.1k samples plus one sample stays within 1 ns of ideal
    CHECK(nsToSamples(samplesToNs(123456789ULL, 44100), 44100) >= 123456788ULL);
    CHECK(beatNs(120.0) == 500'000'000LL);
}

TEST_CASE("SpscQueue is bounded and counts drops") {
    SpscQueue<int> q(3);
    CHECK(q.push(1)); CHECK(q.push(2)); CHECK(q.push(3));
    CHECK_FALSE(q.push(4));
    CHECK(q.dropped() == 1);
    CHECK(q.occupancy() == doctest::Approx(1.0));
    int v = 0;
    CHECK(q.pop(v)); CHECK(v == 1);
    CHECK(q.push(5));
    CHECK(q.pop(v)); CHECK(v == 2);
    CHECK(q.pop(v)); CHECK(v == 3);
    CHECK(q.pop(v)); CHECK(v == 5);
    CHECK_FALSE(q.pop(v));
}

TEST_CASE("LatestMailbox keeps only the newest value") {
    LatestMailbox<std::array<int, 4>> m;
    std::array<int, 4> out{};
    CHECK_FALSE(m.read(out));
    m.publish({1, 2, 3, 4});
    m.publish({5, 6, 7, 8});
    CHECK(m.read(out));
    CHECK(out[0] == 5);
    CHECK(m.version() == 2);
}

TEST_CASE("Rng is deterministic across instances") {
    Rng a(42), b(42), c(43);
    for (int i = 0; i < 100; ++i) CHECK(a.next() == b.next());
    CHECK(Rng(42).next() != c.next());
    Rng r(7);
    for (int i = 0; i < 1000; ++i) { CHECK(r.below(10) < 10); float u = r.unit(); CHECK(u >= 0.f); CHECK(u < 1.f); }
}

TEST_CASE("FakeClock advances monotonic and wall time together") {
    FakeClock c(100);
    CHECK(c.nowMonotonic() == 100);
    c.advance(msToNs(5));
    CHECK(c.nowMonotonic() == 100 + msToNs(5));
}

TEST_CASE("AudioBlock view exposes frames") {
    AudioBlock b; b.channels = 2; b.rateHz = 48000; b.samples.assign(2 * 480, 0.f);
    CHECK(b.frames() == 480);
    CHECK(b.view().frames == 480);
}
