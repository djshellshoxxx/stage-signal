#include "test.h"
#include "perf/isolation.h"
using namespace ss;

TEST(sec_iso_lighting_sink_is_disposable) {
  BenchmarkSinks s; uint8_t d[512] = {0};
  CHECK(!s.lighting.sendTo("127.0.0.1", 6454, d, 512).ok());                        // not open
  CHECK(s.lighting.open("", false).ok());
  CHECK(s.lighting.sendTo("127.0.0.1", 6454, d, 512).ok() && s.lighting.sendTo("localhost", 5568, d, 10).ok() && s.lighting.datagramsSent() == 2 && s.lighting.bytes() == 522);
  CHECK(!s.guard.tripped());
  CHECK(!s.lighting.sendTo("192.168.1.50", 6454, d, 512).ok() && s.guard.tripped() && s.guard.count() == 1);
  CHECK(!s.lighting.sendTo("255.255.255.255", 6454, d, 512).ok() && !s.lighting.sendTo("239.255.0.1", 5568, d, 512).ok() && s.lighting.datagramsSent() == 2);
  BenchmarkSinks t; CHECK(!t.lighting.open("192.168.1.10", false).ok() && t.guard.tripped());
  BenchmarkSinks u; CHECK(!u.lighting.open("", true).ok() && u.guard.tripped());     // broadcast sockets refused
}
TEST(sec_iso_broadcast_never_started) {
  BenchmarkSinks s; Status st = s.requestRemoteBroadcast("youtube");
  CHECK(!st.ok() && st.code == Err::PermissionDenied && !st.recoverable); CHECK(s.guard.tripped() && s.guard.events().size() == 1);
  CHECK(!s.broadcast.begin("rtmps://a.rtmp.youtube.com/live2/key").ok()); CHECK(!s.broadcast.begin("srt://203.0.113.5:9000").ok()); CHECK(!s.broadcast.begin("srt://127.0.0.1.evil.example:9").ok());
  CHECK(!s.broadcast.sendPacket(nullptr, 0).ok()); CHECK(s.broadcast.packets() == 0);
  CHECK(s.broadcast.begin("srt://127.0.0.1:9000").ok() && s.broadcast.begin("discard://null").ok()); CHECK(s.broadcast.sendPacket(nullptr, 0).ok());
  s.guard.reset(); CHECK(!s.guard.tripped());
}
TEST(sec_iso_guard_events_sanitized_and_bounded) {
  NetworkTransmissionForbidden g; for (int i = 0; i < 100; ++i) g.report("x rtmps://u:pw12345@h/app/sk0123456789abcdef " + std::to_string(i));
  CHECK(g.count() == 100 && g.events().size() == 16); for (auto& e : g.events()) CHECK(e.find("pw12345") == std::string::npos && e.find("sk0123456789") == std::string::npos);
}
TEST(sec_iso_record_sink_bounded_and_discarding) {
  DisposableRecordSink r(1000); uint8_t b[600] = {0}; CHECK(r.write(b, 600).ok()); Status s = r.write(b, 600); CHECK(!s.ok() && s.code == Err::DiskFull && r.bytes() == 600);
}
int main() { return ss_test::runAll(); }
