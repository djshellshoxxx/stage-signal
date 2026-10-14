// MOD-07 MediaManager: async worker, bounded queue/cache, placeholder substitution, fault containment, GIF/video clocks.
#include "media_test_util.h"
#include "media/manager.h"
#include "media/placeholder.h"
#include "media/fake_video_decoder.h"
#include <atomic>
#include <random>

using namespace ss; using namespace ss::media;
using mt::png;
using State = MediaHandle::State;

static std::string g_dir;
static std::string dir() { if (g_dir.empty()) g_dir = mt::tmpDir("mgr"); return g_dir; }
static std::string putPng(const std::string& name, int w, int h, mt::PixFn fn = mt::gradient) { std::string p = dir() + "/" + name; mt::writeFile(p, mt::png(w, h, fn)); return p; }
static void reg(MediaManager& m, const std::string& id, const std::string& path, bool loop = true, double speed = 1.0, CropRect crop = CropRect()) {
  AssetSpec a; a.id = id; a.path = path; a.loop = loop; a.speed = speed; a.crop = crop; Status s = m.registerAsset(a); CHECK(s.ok());
  if (!s.ok()) std::printf("  register %s: %s\n", id.c_str(), s.detail.c_str());
}
static bool settle(const MediaHandlePtr& h, int ms = 3000) { return mt::waitFor([&] { return h->state() != State::Pending; }, ms); }
static bool isPlaceholderFrame(const std::shared_ptr<const Frame>& f) {
  static Frame ref = makePlaceholderFrame(320, 240);
  return f && f->w == ref.w && f->h == ref.h && f->px == ref.px;
}
static double elapsedMs(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }

static MediaConfig fastCfg() { MediaConfig c; c.watchdogPollMs = 2; c.workerTimeoutMs = 5000; c.failureRetryMs = 5000; return c; }

TEST(placeholder_is_documented_checkerboard) {
  Frame f = makePlaceholderFrame(320, 240);
  CHECK(f.w == 320 && f.h == 240 && f.px.size() == 320u * 240u);
  CHECK(f.px[0] == kPlaceholderB && f.px[kPlaceholderCell] == kPlaceholderA);                 // cell parity
  CHECK(f.px[(size_t)kPlaceholderCell * 320] == kPlaceholderA);
  size_t white = 0; for (uint32_t p : f.px) if (p == kPlaceholderText) ++white;
  CHECK(white > 200);                                                                          // "MISSING MEDIA" text is present
  CHECK(makePlaceholderFrame(320, 240).px == f.px);                                            // deterministic
  CHECK(makePlaceholderFrame(1, 1).w == 16 && makePlaceholderFrame(99999, 99999).w == 4096);
}

TEST(register_rejects_unsafe_assets) {
  MediaManager m(fastCfg());
  AssetSpec a; a.id = "x";
  for (const char* p : {"http://h/a.png", "https://h/a.gif", "file:///a.png", "\\\\srv\\sh\\a.png", "//srv/a.png", "../a.png", "a/../../b.png", "a.svg", "a.html", "a.js", "a.png:evil", ""}) {
    a.path = p; CHECK(!m.registerAsset(a).ok());
  }
  a.path = "ok.png"; a.id = ""; CHECK(!m.registerAsset(a).ok());
  a.id = "x"; a.crop.w = 0; CHECK(!m.registerAsset(a).ok());
  a.crop = CropRect(); CHECK(m.registerAsset(a).ok());
  MediaHandlePtr h = m.request("never-registered");
  CHECK(h->state() == State::Failed && h->status().code == Err::InvalidArgument && isPlaceholderFrame(h->frame(0)));
}

TEST(still_loads_async_and_caches) {
  MediaManager m(fastCfg());
  reg(m, "a", putPng("a.png", 64, 32));
  MediaHandlePtr h = m.request("a");
  CHECK(h->frame(0) == nullptr || h->state() != State::Pending);                               // never blocks; null while pending
  CHECK(settle(h) && h->state() == State::Ready && h->status().ok());
  auto f = h->frame(0);
  CHECK(f && f->w == 64 && f->h == 32 && h->width() == 64 && h->height() == 32 && h->frameCount() == 1);
  CHECK(f->px[0] == rgb(0, 0, 128) && f->px[63] == rgb(255, 0, 128));
  CHECK(!h->isPlaceholder());
  MediaHandlePtr h2 = m.request("a");                                                          // cache hit: ready immediately
  CHECK(h2->state() == State::Ready && h2->frame(5)->px == f->px);
  CHECK(m.stats().cacheHits == 1 && m.stats().loadsOk == 1 && m.stats().cachedItems == 1);
  CHECK(m.stats().cacheBytes == 64u * 32u * 4u);
}

TEST(base_tier_preprocesses_large_still_off_the_live_path) {
  MediaManager m(fastCfg());
  reg(m, "big", putPng("big.png", 1600, 1200, mt::solidRed));
  MediaHandlePtr h = m.request("big");
  CHECK(settle(h) && h->state() == State::Ready);
  auto f = h->frame(0);
  CHECK(f && f->w == 800 && f->h == 600 && !h->warnings().empty());
  CHECK(m.stats().cacheBytes == 800u * 600u * 4u && m.stats().cacheBytes <= kBaseCacheBudgetBytes);
}

TEST(missing_and_corrupt_media_substitute_placeholder_scene_stays_valid) {
  MediaManager m(fastCfg());
  reg(m, "good", putPng("good.png", 32, 32));
  reg(m, "missing", dir() + "/nope.png");
  std::vector<uint8_t> bad = mt::png(32, 32); bad.resize(bad.size() / 2); mt::writeFile(dir() + "/trunc.png", bad);
  reg(m, "trunc", dir() + "/trunc.png");
  mt::writeFile(dir() + "/evil.png", std::vector<uint8_t>{'<','s','v','g','>'});
  reg(m, "evil", dir() + "/evil.png");
  MediaHandlePtr g = m.request("good"); CHECK(settle(g)); auto goodFrame = g->frame(0);
  for (const char* id : {"missing", "trunc", "evil"}) {
    MediaHandlePtr h = m.request(id);
    CHECK(settle(h) && h->state() == State::Failed && h->isPlaceholder());
    CHECK(!h->status().ok() && h->status().detail.find(dir()) == std::string::npos);        // no directory leaked into messages
    CHECK(isPlaceholderFrame(h->frame(0)));
    CHECK(isPlaceholderFrame(h->frame(123456789)));
  }
  CHECK(g->state() == State::Ready && g->frame(0)->px == goodFrame->px);                       // current scene unaffected
  MediaHandlePtr again = m.request("good"); CHECK(again->state() == State::Ready);
}

TEST(control_thread_never_blocks_while_worker_is_busy) {
  MediaConfig c = fastCfg(); c.queueCapacity = 8;
  MediaManager m(c);
  reg(m, "slow", putPng("slow.png", 16, 16)); reg(m, "fast", putPng("fast.png", 16, 16));
  m.setFaultHook([](const std::string& id) { return id == "slow" ? FaultKind::Hang : FaultKind::None; });
  MediaHandlePtr slow = m.request("slow");
  CHECK(mt::waitFor([&] { return m.stats().queueDepth == 0; }));                              // worker picked it up and is stuck
  mt::sleepMs(20);
  auto t0 = std::chrono::steady_clock::now();
  MediaHandlePtr fast = m.request("fast");
  for (int i = 0; i < 2000; ++i) { (void)fast->frame(i); (void)slow->frame(i); (void)fast->state(); (void)m.stats(); }
  CHECK(elapsedMs(t0) < 150);                                                                  // generous: no decode on this thread
  CHECK(slow->state() == State::Pending && fast->state() == State::Pending && slow->frame(0) == nullptr);
}

TEST(request_queue_is_bounded_and_rejects_overflow) {
  MediaConfig c = fastCfg(); c.queueCapacity = 3;
  MediaManager m(c);
  for (int i = 0; i < 8; ++i) reg(m, "q" + std::to_string(i), putPng("q" + std::to_string(i) + ".png", 8, 8));
  m.setFaultHook([](const std::string& id) { return id == "q0" ? FaultKind::Hang : FaultKind::None; });
  std::vector<MediaHandlePtr> hs;
  hs.push_back(m.request("q0"));
  CHECK(mt::waitFor([&] { return m.stats().queueDepth == 0; }));
  for (int i = 1; i <= 3; ++i) hs.push_back(m.request("q" + std::to_string(i)));              // fills the queue
  CHECK(m.stats().queueDepth == 3);
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 4; i < 8; ++i) hs.push_back(m.request("q" + std::to_string(i)));              // overflow
  CHECK(elapsedMs(t0) < 100);
  CHECK(m.stats().queueDepth == 3 && m.stats().queueRejects == 4);
  for (int i = 4; i < 8; ++i) {
    CHECK(hs[i]->state() == State::Failed && hs[i]->status().code == Err::ResourceExhausted && isPlaceholderFrame(hs[i]->frame(0)));
  }
  CHECK(hs[1]->state() == State::Pending);
}

TEST(cache_budget_is_never_exceeded) {
  MediaConfig c = fastCfg(); c.cacheBudgetBytes = 100000; MediaManager m(c);
  for (int i = 0; i < 5; ++i) reg(m, "s" + std::to_string(i), putPng("s" + std::to_string(i) + ".png", 100, 100));   // 40,000 bytes each
  reg(m, "huge", putPng("huge.png", 300, 300));                                                                 // 360,000 > budget
  for (int i = 0; i < 5; ++i) {
    MediaHandlePtr h = m.request("s" + std::to_string(i)); CHECK(settle(h) && h->state() == State::Ready);
    CHECK(m.stats().cacheBytes <= 100000);
  }
  MediaStats st = m.stats();
  CHECK(st.cachedItems == 2 && st.evictions == 3 && st.cacheBytes == 80000 && st.cachePeakBytes <= 100000);
  MediaHandlePtr s4 = m.request("s4"); CHECK(s4->state() == State::Ready && m.stats().cacheHits == 1);            // most recent survived (LRU)
  MediaHandlePtr s0 = m.request("s0"); CHECK(s0->state() == State::Pending || s0->state() == State::Ready);       // oldest was evicted -> reload
  CHECK(settle(s0));
  MediaHandlePtr huge = m.request("huge"); CHECK(settle(huge) && huge->state() == State::Failed && huge->status().code == Err::ResourceExhausted);
  CHECK(isPlaceholderFrame(huge->frame(0)) && m.stats().cachePeakBytes <= 100000);
  // pinned payloads are not evictable: while handles hold s0+s4 (80,000), a third 40,000 cannot be admitted
  MediaHandlePtr s1 = m.request("s1"); CHECK(settle(s1));
  CHECK(s1->state() == State::Failed && s1->status().code == Err::ResourceExhausted);
  CHECK(m.stats().cacheBytes <= 100000 && m.stats().cachePeakBytes <= 100000);
  s4.reset(); s0.reset();                                                                                       // unpin -> eviction possible again
  m.invalidate("s1"); MediaHandlePtr s1b = m.request("s1"); CHECK(settle(s1b) && s1b->state() == State::Ready);
  CHECK(m.stats().cachePeakBytes <= 100000);
}

TEST(cache_budget_randomized_stress) {
  MediaConfig c = fastCfg(); c.cacheBudgetBytes = 150000; c.failureRetryMs = 1; MediaManager m(c);
  int sizes[] = {20, 60, 100, 150, 200};
  for (int i = 0; i < 5; ++i) reg(m, "r" + std::to_string(i), putPng("r" + std::to_string(i) + ".png", sizes[i], sizes[i]));
  std::mt19937 rng(7); std::vector<MediaHandlePtr> held; std::vector<std::shared_ptr<const Frame>> frames;
  for (int it = 0; it < 120; ++it) {
    MediaHandlePtr h = m.request("r" + std::to_string(rng() % 5));
    settle(h);
    if (rng() % 3 == 0) { held.push_back(h); if (held.size() > 3) held.erase(held.begin()); }
    if (rng() % 4 == 0) frames.push_back(h->frame(0));
    if (frames.size() > 2) frames.erase(frames.begin());
    mt::sleepMs(1);
    CHECK(m.stats().cacheBytes <= 150000);
  }
  CHECK(m.stats().cachePeakBytes <= 150000);
  held.clear(); frames.clear();
}

TEST(decoder_exceptions_are_contained_and_source_substituted) {
  MediaManager m(fastCfg());
  for (const char* id : {"ex1", "ex2", "ex3", "fine"}) reg(m, id, putPng(std::string(id) + ".png", 16, 16));
  m.setFaultHook([](const std::string& id) {
    return id == "ex1" ? FaultKind::Throw : id == "ex2" ? FaultKind::ThrowNonStd : id == "ex3" ? FaultKind::BadAlloc : FaultKind::None; });
  Err want[] = {Err::DecodeFailure, Err::InternalFault, Err::ResourceExhausted}; int i = 0;
  for (const char* id : {"ex1", "ex2", "ex3"}) {
    MediaHandlePtr h = m.request(id);
    CHECK(settle(h) && h->state() == State::Failed && h->status().code == want[i++] && isPlaceholderFrame(h->frame(0)));
  }
  MediaHandlePtr ok = m.request("fine"); CHECK(settle(ok) && ok->state() == State::Ready);
  CHECK(m.stats().workerRestarts == 0 && m.stats().loadsFailed == 3);
}

TEST(worker_death_is_detected_substituted_and_worker_restarted) {
  MediaManager m(fastCfg());
  for (const char* id : {"die", "next", "after"}) reg(m, id, putPng(std::string(id) + ".png", 16, 16));
  std::atomic<int> calls{0};
  m.setFaultHook([&](const std::string& id) { ++calls; return id == "die" ? FaultKind::WorkerDie : FaultKind::None; });
  MediaHandlePtr d = m.request("die");
  MediaHandlePtr queued = m.request("next");                                                   // queued behind the dying job
  CHECK(settle(d) && d->state() == State::Failed && d->status().code == Err::InternalFault && isPlaceholderFrame(d->frame(0)));
  CHECK(settle(queued) && queued->state() == State::Ready);                                    // restarted worker drains the queue
  MediaHandlePtr a = m.request("after"); CHECK(settle(a) && a->state() == State::Ready);
  MediaStats st = m.stats(); CHECK(st.workerRestarts == 1 && st.workerDeaths == 1 && st.hungJobs == 0);
}

TEST(hung_decoder_times_out_source_substituted_manager_keeps_running) {
  MediaConfig c = fastCfg(); c.workerTimeoutMs = 40; MediaManager m(c);
  for (const char* id : {"hang", "b", "c"}) reg(m, id, putPng(std::string(id) + ".png", 16, 16));
  m.setFaultHook([](const std::string& id) { return id == "hang" ? FaultKind::Hang : FaultKind::None; });
  MediaHandlePtr hh = m.request("hang");
  MediaHandlePtr b = m.request("b");
  auto t0 = std::chrono::steady_clock::now();
  CHECK(settle(hh, 2000) && hh->state() == State::Failed && isPlaceholderFrame(hh->frame(0)));
  CHECK(elapsedMs(t0) < 1500);
  CHECK(settle(b) && b->state() == State::Ready);
  MediaHandlePtr c2 = m.request("c"); CHECK(settle(c2) && c2->state() == State::Ready);
  CHECK(m.stats().hungJobs == 1 && m.stats().workerRestarts == 1);
  // a hang is negative-cached: the same source is not retried immediately
  MediaHandlePtr again = m.request("hang"); CHECK(again->state() == State::Failed);
}

TEST(repeated_manager_lifecycle_with_inflight_requests_does_not_hang_or_leak) {
  for (int i = 0; i < 15; ++i) {
    MediaConfig c = fastCfg(); c.workerTimeoutMs = 30; MediaManager m(c);
    reg(m, "a", putPng("lc.png", 64, 64)); reg(m, "h", putPng("lc2.png", 16, 16));
    m.setFaultHook([](const std::string& id) { return id == "h" ? FaultKind::Hang : FaultKind::None; });
    MediaHandlePtr h = m.request("h"); MediaHandlePtr a = m.request("a");
    if (i % 3 == 0) mt::sleepMs(2);
  }
  CHECK(true);
}

TEST(failed_sources_are_negative_cached_then_retried) {
  MediaConfig c = fastCfg(); c.failureRetryMs = 80; MediaManager m(c);
  std::string path = dir() + "/late.png"; reg(m, "late", path);
  MediaHandlePtr h = m.request("late"); CHECK(settle(h) && h->state() == State::Failed);
  uint64_t failed = m.stats().loadsFailed;
  MediaHandlePtr h2 = m.request("late"); CHECK(h2->state() == State::Failed);                // answered from the failure record
  CHECK(m.stats().loadsFailed == failed && m.stats().queueDepth == 0);
  mt::writeFile(path, mt::png(8, 8));
  mt::sleepMs(100);
  MediaHandlePtr h3 = m.request("late"); CHECK(settle(h3) && h3->state() == State::Ready);   // retry window elapsed, file now exists
}

// ---- GIF on the media clock ---------------------------------------------------------------------------------------
static std::string putGif(const std::string& name, int n, int delayCs, int loop = 0) {
  ss_gif::Spec s; s.w = 8; s.h = 8; s.palette = {255,0,0, 0,255,0, 0,0,255, 255,255,0}; s.loop = loop;
  for (int i = 0; i < n; ++i) s.frames.push_back(mt::gframe(0, 0, 8, 8, i % 4, delayCs, 1));
  std::string p = dir() + "/" + name; mt::writeFile(p, ss_gif::build(s)); return p;
}
static uint32_t gifColor(int i) { static const uint32_t c[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00}; return c[i % 4]; }

TEST(gif_uses_media_clock_speed_loop) {
  MediaManager m(fastCfg());
  reg(m, "g", putGif("g.gif", 4, 10));                                                          // 4 frames x 100 ms
  MediaHandlePtr h = m.request("g"); CHECK(settle(h) && h->state() == State::Ready && h->frameCount() == 4);
  CHECK(h->frame(0)->px[0] == gifColor(0));
  CHECK(h->frame(99 * kNsPerMs)->px[0] == gifColor(0));
  CHECK(h->frame(100 * kNsPerMs)->px[0] == gifColor(1));
  CHECK(h->frame(250 * kNsPerMs)->px[0] == gifColor(2));
  CHECK(h->frame(399 * kNsPerMs)->px[0] == gifColor(3));
  CHECK(h->frame(400 * kNsPerMs)->px[0] == gifColor(0));                                        // loop wraps
  CHECK(h->frame(1250 * kNsPerMs)->px[0] == gifColor(0 + (1250 % 400) / 100));                  // independent of how often we sample
  // same time via many render ticks or one: frame depends only on the media clock
  MediaHandlePtr h2 = m.request("g"); h2->frame(0); h2->frame(1); h2->frame(2);
  CHECK(h2->frame(250 * kNsPerMs)->px[0] == gifColor(2));
  // speed 2x
  MediaHandlePtr fast = m.request("g"); fast->setSpeed(2.0); fast->frame(0);
  CHECK(fast->frame(100 * kNsPerMs)->px[0] == gifColor(2));                                     // 100 ms real = 200 ms media
  // speed clamps to 4x (not 100x) and to >=0.25x
  MediaHandlePtr cl = m.request("g"); cl->setSpeed(100.0); cl->frame(0);
  CHECK(cl->frame(110 * kNsPerMs)->px[0] == gifColor(0));                                       // 440 ms mod 400 = 40 -> frame 0 (100x would give frame 2)
  MediaHandlePtr slow = m.request("g"); slow->setSpeed(0.001); slow->frame(0);
  CHECK(slow->frame(400 * kNsPerMs)->px[0] == gifColor(1));                                     // 0.25x: 100 ms media
  // changing speed mid-stream does not jump
  MediaHandlePtr ch = m.request("g"); ch->frame(0); ch->frame(100 * kNsPerMs); ch->setSpeed(2.0);
  CHECK(ch->frame(150 * kNsPerMs)->px[0] == gifColor(2));                                       // 100 + 50*2 = 200 ms
  // no loop holds the last frame
  ch->setLoop(false); CHECK(ch->frame(10 * kNsPerMs * 1000)->px[0] == gifColor(3));
  // the frame pointer stays valid after the handle and cache entry are gone
  std::shared_ptr<const Frame> kept = m.request("g")->frame(0);
  m.invalidate("g");
  CHECK(kept->w == 8 && kept->px[0] == gifColor(0));
}

TEST(gif_warnings_crop_and_failure) {
  MediaManager m(fastCfg());
  { ss_gif::Spec s; s.w = 8; s.h = 8; s.palette = {255,0,0, 0,255,0}; s.frames = {mt::gframe(0,0,8,8,0,0,1), mt::gframe(0,0,8,8,1,1,1)};
    mt::writeFile(dir() + "/fastdelay.gif", ss_gif::build(s)); }
  CropRect half; half.x = 0; half.y = 0; half.w = 0.5f; half.h = 0.5f;
  reg(m, "fd", dir() + "/fastdelay.gif", true, 1.0, half);
  MediaHandlePtr h = m.request("fd"); CHECK(settle(h) && h->state() == State::Ready);
  CHECK(h->warnings().size() == 1 && h->warnings()[0].find("20 ms") != std::string::npos);
  CHECK(h->frame(0)->w == 4 && h->frame(0)->h == 4);
  CHECK(h->frame(19 * kNsPerMs)->px[0] == 0xFF0000 && h->frame(20 * kNsPerMs)->px[0] == 0x00FF00);     // 0 and 10 ms delays became 20 ms each
  mt::writeFile(dir() + "/broken.gif", std::vector<uint8_t>{'G','I','F','8','9','a', 8, 0, 8, 0, 0x80, 0, 0});
  reg(m, "broken", dir() + "/broken.gif");
  MediaHandlePtr b = m.request("broken"); CHECK(settle(b) && b->state() == State::Failed && isPlaceholderFrame(b->frame(0)));
  // BASE: GIF whose decoded size exceeds the 8 MiB cache is refused before decoding
  { ss_gif::Spec s; s.w = 640; s.h = 480; s.palette = {255,0,0, 0,255,0}; for (int i = 0; i < 8; ++i) s.frames.push_back(mt::gframe(0,0,8,8,i % 2,5,1));
    mt::writeFile(dir() + "/heavy.gif", ss_gif::build(s)); }                                  // 640*480*4*8 = 9.8 MB
  reg(m, "heavy", dir() + "/heavy.gif");
  MediaHandlePtr hv = m.request("heavy"); CHECK(settle(hv) && hv->state() == State::Failed && hv->status().code == Err::ResourceExhausted);
  CHECK(m.stats().cachePeakBytes <= kBaseCacheBudgetBytes);
}

// ---- video ----------------------------------------------------------------------------------------------------------
static std::string putVideo(const std::string& name, const char* fourcc) {
  std::vector<uint8_t> v = {0,0,0,24,'f','t','y','p','i','s','o','m',0,0,0,0,'i','s','o','m'};
  for (int i = 0; i < 4; ++i) v.push_back((uint8_t)fourcc[i]);
  for (int i = 0; i < 64; ++i) v.push_back(0);
  std::string p = dir() + "/" + name; mt::writeFile(p, v); return p;
}
static const char* kEnabled = R"({"decoders":[{"id":"fake-h264","enabled":true,"containers":["mp4"],"codecs":["h264"]}]})";
static DecoderRegistry makeRegistry(FakeVideoParams p, std::shared_ptr<FakeVideoCounters> c, const char* manifest = kEnabled) {
  DecoderRegistry r; DecoderEntry e; e.id = "fake-h264"; e.containers = {"mp4", "mov"}; e.codecs = {"h264"};
  e.factory = [p, c]() { return std::unique_ptr<IVideoDecoder>(new FakeVideoDecoder(p, c)); };
  r.registerDecoder(e); if (manifest) CHECK(r.loadManifestAvailability(manifest).ok());
  return r;
}
static MediaConfig stdCfg() { MediaConfig c = fastCfg(); c.tier = Tier::Standard; return c; }
static int redOf(const std::shared_ptr<const Frame>& f) { return f ? (int)((f->px[0] >> 16) & 255) : -1; }

TEST(decoder_availability_comes_from_manifest_not_filename) {
  auto c = std::make_shared<FakeVideoCounters>();
  DecoderRegistry none = makeRegistry(FakeVideoParams(), c, nullptr);
  CHECK(!none.isAvailable("mp4", "h264"));                                                     // no manifest loaded -> fail closed
  CHECK(none.report().size() == 1 && !none.report()[0].available && none.report()[0].compiled);
  DecoderRegistry off = makeRegistry(FakeVideoParams(), c, R"({"decoders":[{"id":"fake-h264","enabled":false}]})");
  CHECK(!off.isAvailable("mp4", "h264") && !off.report()[0].enabledByManifest);
  DecoderRegistry on = makeRegistry(FakeVideoParams(), c);
  CHECK(on.isAvailable("mp4", "h264") && on.isAvailable("mov", "h264") && !on.isAvailable("mkv", "h264") && !on.isAvailable("mp4", "h265"));
  CHECK(on.create("mp4", "h264") != nullptr && on.create("mp4", "av1") == nullptr);
  DecoderRegistry ghost = makeRegistry(FakeVideoParams(), c, R"({"decoders":[{"id":"fake-h264","enabled":true},{"id":"not-compiled","enabled":true}]})");
  auto rep = ghost.report(); CHECK(rep.size() == 2 && !rep[1].available && rep[1].reason.find("not compiled") != std::string::npos);
  CHECK(!on.loadManifestAvailability("{ not json").ok());
  // codec comes from the container bytes: an .mp4 whose sample entry is HEVC is not served by the H.264 decoder
  std::string avc = putVideo("a.mp4", "avc1"), hevc = putVideo("b.mp4", "hvc1");
  std::vector<uint8_t> a, b; readFileHead(avc, 4096, a); readFileHead(hevc, 4096, b);
  CHECK(probeVideoCodec(a.data(), a.size(), Container::Mp4Mov) == "h264" && probeVideoCodec(b.data(), b.size(), Container::Mp4Mov) == "h265");
  CHECK(probeVideoCodec(a.data(), 8, Container::Mp4Mov).empty());
}

TEST(video_is_gated_by_tier_manifest_and_content) {
  auto c = std::make_shared<FakeVideoCounters>();
  { MediaManager base(fastCfg()); reg(base, "v", putVideo("v.mp4", "avc1")); base.setDecoderRegistry(makeRegistry(FakeVideoParams(), c));
    MediaHandlePtr h = base.request("v"); CHECK(h->state() == State::Failed && h->status().code == Err::UnsupportedCapability); }   // BASE: no video
  MediaManager m(stdCfg());
  reg(m, "v", putVideo("v.mp4", "avc1")); reg(m, "hevc", putVideo("hevc.mp4", "hvc1")); reg(m, "disguised", putVideo("d.mp4", "avc1"));
  mt::writeFile(dir() + "/fake.mp4", mt::png(8, 8)); reg(m, "pngasmp4", dir() + "/fake.mp4");
  MediaHandlePtr nm = m.request("v"); CHECK(settle(nm) && nm->status().code == Err::UnsupportedCapability);   // no registry installed
  m.invalidate("v");
  m.setDecoderRegistry(makeRegistry(FakeVideoParams(), c, R"({"decoders":[{"id":"fake-h264","enabled":false}]})"));
  MediaHandlePtr dis = m.request("v"); CHECK(settle(dis) && dis->status().code == Err::UnsupportedCapability);
  m.invalidate("v");
  m.setDecoderRegistry(makeRegistry(FakeVideoParams(), c));
  MediaHandlePtr hv = m.request("hevc"); CHECK(settle(hv) && hv->state() == State::Failed && hv->status().code == Err::UnsupportedCapability);
  MediaHandlePtr pm = m.request("pngasmp4"); CHECK(settle(pm) && pm->state() == State::Failed && isPlaceholderFrame(pm->frame(0)));   // magic mismatch
  CHECK(c->opened == 0);
}

TEST(video_clock_speed_loop_crop_and_release) {
  auto c = std::make_shared<FakeVideoCounters>();
  FakeVideoParams p; p.w = 64; p.h = 48; p.frames = 30; p.fps = 10;                              // 3.0 s, frame i has red = i
  MediaManager m(stdCfg()); m.setDecoderRegistry(makeRegistry(p, c));
  reg(m, "v", putVideo("v.mp4", "avc1"));
  MediaHandlePtr h = m.request("v");
  CHECK(settle(h) && h->state() == State::Ready);
  CHECK(mt::waitFor([&] { return redOf(h->frame(0)) == 0; }));
  CHECK(h->width() == 64 && h->height() == 48);
  CHECK(mt::waitFor([&] { return redOf(h->frame(250 * kNsPerMs)) == 2; }));                      // newest frame with pts <= 0.25 s
  CHECK(mt::waitFor([&] { return redOf(h->frame(1000 * kNsPerMs)) == 10; }));
  CHECK(mt::waitFor([&] { return redOf(h->frame(3250 * kNsPerMs)) == 2; }));                     // looped past 3.0 s
  CHECK(c->seeks >= 1 && c->opened == 1);
  CHECK(m.stats().activeVideos == 1 && m.stats().cacheBytes == 3ull * 800 * 600 * 4);          // reserved decoder buffers counted
  // only one video source in STANDARD admission
  reg(m, "v2", putVideo("v2.mp4", "avc1"));
  MediaHandlePtr second = m.request("v2"); CHECK(second->state() == State::Failed && second->status().code == Err::ResourceExhausted);
  // releasing the handle closes the decoder on the worker thread and returns the reservation
  h.reset();
  CHECK(mt::waitFor([&] { return c->closed == 1 && m.stats().activeVideos == 0 && m.stats().cacheBytes == 0; }));

  // speed 2x, no loop (hold last), crop to the right half
  CropRect right; right.x = 0.5f; right.y = 0; right.w = 0.5f; right.h = 1;
  AssetSpec a; a.id = "vs"; a.path = dir() + "/v.mp4"; a.loop = false; a.speed = 2.0; a.crop = right; CHECK(m.registerAsset(a).ok());
  MediaHandlePtr s = m.request("vs"); CHECK(settle(s));
  CHECK(mt::waitFor([&] { auto f = s->frame(0); return f && f->w == 32 && f->h == 48; }));
  CHECK(mt::waitFor([&] { return redOf(s->frame(150 * kNsPerMs)) == 3; }));                      // 150 ms real * 2 = 300 ms media
  auto f = s->frame(150 * kNsPerMs); CHECK(f && ((f->px[0] >> 8) & 255) >= 127 && ((f->px[0] >> 8) & 255) <= 129);   // crop starts at x = 32 of 64 (green gradient ~128)
  CHECK(mt::waitFor([&] { return redOf(s->frame(100000 * kNsPerMs)) == 29; }));                  // end: holds last frame
  mt::sleepMs(20); CHECK(redOf(s->frame(200000 * kNsPerMs)) == 29);
  s->setSpeed(50.0); s->frame(0);                                                                // clamp to 2x for video
  CHECK(s->state() == State::Ready);
}

TEST(video_decoder_fault_substitutes_source_and_manager_continues) {
  auto c = std::make_shared<FakeVideoCounters>();
  FakeVideoParams p; p.throwAtFrame = 5;
  MediaManager m(stdCfg()); m.setDecoderRegistry(makeRegistry(p, c));
  reg(m, "v", putVideo("v.mp4", "avc1")); reg(m, "img", putPng("vimg.png", 16, 16));
  MediaHandlePtr v = m.request("v"); CHECK(settle(v));
  CHECK(mt::waitFor([&] { v->frame(2 * kNsPerSec); return v->state() == State::Failed; }));
  CHECK(isPlaceholderFrame(v->frame(0)) && v->status().code == Err::DecodeFailure);
  MediaHandlePtr img = m.request("img"); CHECK(settle(img) && img->state() == State::Ready);
  CHECK(mt::waitFor([&] { return c->closed == 1 && m.stats().activeVideos == 0; }));
  CHECK(m.stats().workerRestarts == 0);
  // open failure
  FakeVideoParams q; q.failOpen = true; m.setDecoderRegistry(makeRegistry(q, c)); m.invalidate("v");
  MediaHandlePtr o = m.request("v"); CHECK(settle(o) && o->state() == State::Failed && isPlaceholderFrame(o->frame(0)));
}

TEST(video_decoder_hang_is_caught_by_watchdog) {
  static std::atomic<bool> release; release = false;
  auto c = std::make_shared<FakeVideoCounters>();
  FakeVideoParams p; p.beforeFrame = [](int idx) { if (idx == 3) { while (!release.load()) mt::sleepMs(1); } };   // non-cooperative hang
  MediaConfig cfg = stdCfg(); cfg.workerTimeoutMs = 60;
  {
    MediaManager m(cfg); m.setDecoderRegistry(makeRegistry(p, c));
    reg(m, "v", putVideo("v.mp4", "avc1")); reg(m, "img", putPng("himg.png", 16, 16));
    MediaHandlePtr v = m.request("v"); CHECK(settle(v));
    CHECK(mt::waitFor([&] { v->frame(5 * kNsPerSec); return v->state() == State::Failed; }, 4000));
    CHECK(isPlaceholderFrame(v->frame(0)));
    MediaHandlePtr img = m.request("img"); CHECK(settle(img) && img->state() == State::Ready);   // fresh worker serves other media
    CHECK(m.stats().hungJobs == 1 && m.stats().workerRestarts == 1);
    release = true;                                                                              // let the orphaned thread finish before teardown
  }
}

int main() { return ss_test::runAll(); }
