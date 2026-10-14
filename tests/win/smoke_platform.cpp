// Windows adapter smoke test (console). Enumerates devices and exercises pure logic + loopback paths.
// Exits 0 when no hardware is present (headless/wine); exits 1 only if deterministic self-checks fail.
// Build: tests/win/build_win.sh  (NOT part of the CMake test glob).
#include "platform/windows/platform.h"
#include "platform/windows/display_ident.h"
#include "platform/windows/pixconv.h"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

using namespace ss;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define INFO(...) do { std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

static void testPure() {
  INFO("[pure] pixel conversion / EDID / paths");
  // YUY2: grey 128 -> mid grey; NV12 likewise
  uint8_t yuy2[4] = {128, 128, 128, 128}; uint32_t px[2];
  win::pix::yuy2ToRgb32(yuy2, 4, 2, 1, false, px);
  CHECK(((px[0] >> 16) & 255) > 120 && ((px[0] >> 16) & 255) < 135);
  CHECK(win::pix::yuvToRgb(16, 128, 128) == 0 && win::pix::yuvToRgb(235, 128, 128) >= 0xF8F8F8u);
  uint8_t y[4] = {16, 235, 16, 235}, uv[2] = {128, 128}; uint32_t o[4];
  win::pix::nv12ToRgb32(y, 2, uv, 2, 2, 2, o);
  CHECK(o[0] == 0 && o[1] >= 0xF8F8F8u);
  uint8_t bgr[6] = {1, 2, 3, 4, 5, 6}; uint32_t b[2];
  win::pix::bgr24ToRgb32(bgr, 6, 2, 1, false, b);
  CHECK(b[0] == 0x030201u && b[1] == 0x060504u);
  int ow, oh; win::pix::fitSize(1920, 1080, 640, 480, ow, oh);
  CHECK(ow == 640 && oh == 360); win::pix::fitSize(320, 240, 640, 480, ow, oh); CHECK(ow == 320 && oh == 240);
  uint32_t src[4] = {1, 2, 3, 4}, dst[1]; win::pix::scaleNearest(src, 2, 2, dst, 1, 1); CHECK(dst[0] != 0);

  uint8_t edid[128] = {0};
  const uint8_t hdr[8] = {0, 255, 255, 255, 255, 255, 255, 0}; std::memcpy(edid, hdr, 8);
  edid[8] = 0x10; edid[9] = 0xAC;                     // 'D''E''L'
  edid[10] = 0x96; edid[11] = 0x40; edid[12] = 0x39; edid[54 + 3] = 0xFC; std::memcpy(edid + 54 + 5, "TestMon\n", 8);
  edid[54] = edid[55] = edid[56] = 0; edid[54 + 18 - 18] = 0;
  win::EdidInfo e; CHECK(win::parseEdid(edid, 128, e)); CHECK(e.mfg == "DEL"); CHECK(e.name == "TestMon"); CHECK(e.product == 0x4096);
  edid[0] = 1; CHECK(!win::parseEdid(edid, 128, e));
  std::string model, inst;
  CHECK(win::splitInterfacePath("\\\\?\\DISPLAY#DEL4096#5&abc&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}", model, inst));
  CHECK(model == "DEL4096" && inst == "5&abc&0&UID4352");
  CHECK(!win::splitInterfacePath("garbage", model, inst));
}

static void testText() {
  INFO("[text] GDI rasterizer");
  auto t = win::makeTextRasterizer();
  TextStyle st; st.px = 24; st.color = 0xFF8000;
  int w = 0, h = 0; std::vector<uint32_t> a;
  std::u32string s = U"Hello wrapped world \U0001F600 \u00E9\u4E2D\u6587 this line should wrap onto multiple lines";
  bool ok = t->rasterize(s, st, 200, w, h, a);
  INFO("  rasterize -> %s %dx%d", ok ? "ok" : "FAILED", w, h);
  CHECK(ok); if (!ok) return;
  CHECK(w > 0 && w <= 200 && h > 24 * 2 && a.size() == (size_t)w * h);
  size_t lit = 0; bool colorOk = true;
  for (uint32_t p : a) if (p >> 24) { ++lit; if ((p & 0xFFFFFF) != 0xFF8000) colorOk = false; }
  CHECK(lit > 50); CHECK(colorOk);
  std::u32string bad; bad.push_back(0xD800); bad.push_back(0x110000); bad += U"ok";
  CHECK(t->rasterize(bad, st, 300, w, h, a));        // invalid scalars replaced, not rejected
  CHECK(!t->rasterize(U"x", st, 0, w, h, a));
}

static void testUdp() {
  INFO("[net] loopback UDP + sink");
  auto rx = win::makeUdpSocket();
  Status s = rx->bind("127.0.0.1", 0, false, false);
  CHECK(s.ok()); if (!s.ok()) { INFO("  bind: %s", s.detail.c_str()); return; }
  auto sink = win::makeDatagramSink();
  CHECK(sink->open("127.0.0.1", false).ok());
  const uint8_t pkt[5] = {1, 2, 3, 4, 5};
  CHECK(sink->sendTo("127.0.0.1", rx->localPort(), pkt, 5).ok());
  uint8_t buf[16]; std::string from; uint16_t fp = 0;
  int n = rx->recvFrom(buf, sizeof buf, 1000, &from, &fp);
  CHECK(n == 5 && std::memcmp(buf, pkt, 5) == 0 && from == "127.0.0.1");
  CHECK(sink->datagramsSent() == 1);
  CHECK(!sink->sendTo("255.255.255.255", 6454, pkt, 5).ok());      // broadcast refused unless opted in
  CHECK(rx->recvFrom(buf, sizeof buf, 20, nullptr, nullptr) == 0);
  INFO("  NICs: %zu", win::listNics().size());
}

static void testSecrets() {
  INFO("[secret] DPAPI round trip");
  auto st = win::makeSecretStore();
  Status p = st->put("smoke-test-ref", "s3cr3t-value");
  if (!p.ok()) { INFO("  SKIP (put failed: %s) - DPAPI/profile unavailable here", p.detail.c_str()); return; }
  std::string out; CHECK(st->get("smoke-test-ref", out).ok()); CHECK(out == "s3cr3t-value");
  CHECK(!st->get("smoke-test-other", out).ok());
  CHECK(st->remove("smoke-test-ref").ok()); CHECK(!st->get("smoke-test-ref", out).ok());
}

static void testDevices() {
  INFO("[devices] enumeration (none is fine)");
  INFO("  WASAPI available: %d, MediaFoundation available: %d", (int)win::wasapiAvailable(), (int)win::mediaFoundationAvailable());
  {
    auto a = win::makeAudioCapture(win::AudioBackend::Auto);
    for (auto& d : a->enumerate()) INFO("  audio(auto): %s | %s | def=%d ch=%u rate=%u", d.id.c_str(), d.name.c_str(), (int)d.isDefault, d.channels, d.rate);
    auto w = win::makeAudioCapture(win::AudioBackend::WinMM);
    auto list = w->enumerate();
    for (auto& d : list) INFO("  audio(winmm): %s | %s", d.id.c_str(), d.name.c_str());
    AudioOpenParams bogus; bogus.deviceId = "winmm:no-such-device|0000.0000#0";
    Status s = w->open(bogus); CHECK(!s.ok()); CHECK(w->status().state == AudioState::Unavailable);   // never falls back to another device
    w->close(); CHECK(w->status().state == AudioState::Closed);
    bogus.deviceId = "{0.0.1.00000000}.{00000000-0000-0000-0000-000000000000}";
    s = a->open(bogus); CHECK(!s.ok());
    a->close();
  }
  {
    auto d = win::makeDisplayProvider();
    auto list = d->enumerate();
    for (auto& x : list) INFO("  display %d: %s | %s | %dx%d@%d,%d dpi=%d rot=%d%s%s", x.index, x.stableId.c_str(), x.name.c_str(), x.w, x.h, x.x, x.y, x.dpi, x.rotation, x.primary ? " primary" : "", x.ambiguous ? " AMBIGUOUS" : "");
    d->setChangeCallback([] {});
  }
  {
    auto c = win::makeCamera();
    auto list = c->enumerate();
    for (auto& x : list) INFO("  camera: %s | %s", x.id.c_str(), x.name.c_str());
    Status s = c->open("\\\\?\\no-such-camera", CameraFormat());
    CHECK(!s.ok());                                    // never auto-selects an alternate
    INFO("  open(bogus camera) -> %s: %s", errName(s.code), s.detail.c_str());
    auto v = win::makeVideoDecoder();
    s = v->open("C:\\definitely\\missing.mp4", 320, 240); CHECK(!s.ok());
    INFO("  video open(missing) -> %s", errName(s.code));
  }
  {
    auto m = win::makeMidiPort();
    int in = 0, out = 0;
    for (auto& p : m->enumerate()) { INFO("  midi %s: %s | %s", p.input ? "in " : "out", p.stableId.c_str(), p.name.c_str()); (p.input ? in : out)++; }
    Status s = m->openInput("in:nonexistent|0000.0000#0", nullptr); CHECK(!s.ok());
    s = m->send(reinterpret_cast<const uint8_t*>("\x90\x40\x7f"), 3); CHECK(!s.ok());
    INFO("  midi ports in=%d out=%d", in, out);
  }
}

static void testSys() {
  INFO("[sys] info and counters");
  INFO("  OS: %s", win::windowsVersionString().c_str());
  INFO("  CPU: %s", win::cpuModelString().c_str());
  INFO("  Power plan: %s", win::powerPlanName().c_str());
  win::SysCounters c; bool ok = win::readSysCounters(c);
  INFO("  counters valid=%d cores=%u perCore=%d ws=%llu private=%llu physTotal=%llu", (int)ok, c.logicalCores, (int)c.perCoreAvailable,
       (unsigned long long)c.workingSetBytes, (unsigned long long)c.privateBytes, (unsigned long long)c.physTotalBytes);
  CHECK(c.logicalCores >= 1);
  CHECK(c.wallNs > 0);
}

int main() {
  testPure();
  testSys();
  testText();
  testUdp();
  testSecrets();
  testDevices();
  INFO("smoke: %s (%d failed checks)", g_fail ? "FAILURES" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
