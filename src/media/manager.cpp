#include "media/manager.h"
#include "media/animation.h"
#include "media/placeholder.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace ss { namespace media {
namespace detail {

struct Accounting {
  std::atomic<uint64_t> live{0}, peak{0};
  uint64_t budget = 0;
  void add(uint64_t b) { uint64_t v = live.fetch_add(b) + b; uint64_t p = peak.load(); while (v > p && !peak.compare_exchange_weak(p, v)) {} }
  void sub(uint64_t b) { live.fetch_sub(b); }
};

// Immutable decoded still/GIF. Its bytes are counted in Accounting from admission until the last reference dies
// (so payloads pinned by handles after LRU eviction still count toward the budget).
struct Payload {
  bool anim = false; int w = 0, h = 0;
  Frame still; std::vector<Frame> frames; std::vector<Ns> ends;
  std::vector<std::string> warnings;
  uint64_t bytes = 0; bool accounted = false; std::shared_ptr<Accounting> acct;
  ~Payload() { if (accounted && acct) acct->sub(bytes); }
};

struct VideoSession {
  std::string assetId; std::unique_ptr<IVideoDecoder> dec; CropRect crop;
  std::atomic<int64_t> targetNs{0};            // speed-scaled media time requested by the consumer
  std::atomic<bool> failed{false}, released{false}, loop{true};
  std::mutex mb; std::shared_ptr<const Frame> latest; Status failStatus;
  uint64_t reserved = 0; std::shared_ptr<Accounting> acct;
  // worker-only decode state
  Frame pending; Ns pendingPts = 0; bool havePending = false; Ns loopBase = 0; Ns lastPts = 0; Ns frameDur = 100 * kNsPerMs; bool ended = false;
  int w = 0, h = 0;
  ~VideoSession() { if (reserved && acct) acct->sub(reserved); }
};

struct Ticket {
  std::string assetId;
  std::atomic<int> done{0};                    // 0 pending, 1 completed (CAS: worker vs watchdog)
  std::atomic<int> handles{0};
  mutable std::mutex m; MediaHandle::State st = MediaHandle::State::Pending; Status status;
  std::shared_ptr<const Payload> payload; std::shared_ptr<VideoSession> video;
  bool complete(MediaHandle::State s, Status stt, std::shared_ptr<const Payload> p, std::shared_ptr<VideoSession> v) {
    int exp = 0; if (!done.compare_exchange_strong(exp, 1)) return false;
    std::lock_guard<std::mutex> lk(m); st = s; status = std::move(stt); payload = std::move(p); video = std::move(v); return true;
  }
};

struct Job { AssetSpec spec; AssetKind kind; std::shared_ptr<Ticket> ticket; };

struct WorkerCtl {
  std::thread t; std::atomic<bool> abandoned{false}, exited{false};
  ~WorkerCtl() { if (t.joinable()) t.detach(); }
};

struct Impl : std::enable_shared_from_this<Impl> {
  MediaConfig cfg; const IClock* clock = nullptr; std::shared_ptr<Accounting> acct = std::make_shared<Accounting>();
  uint64_t budget = 0;
  std::mutex m; std::condition_variable cv, cvHang, cvSup;
  std::unordered_map<std::string, std::pair<AssetSpec, AssetKind>> assets;
  std::deque<Job> queue;
  std::map<std::string, std::shared_ptr<Ticket>> inflight;        // loading, or live video session
  struct CacheEntry { std::shared_ptr<const Payload> p; std::list<std::string>::iterator it; };
  std::map<std::string, CacheEntry> cache; std::list<std::string> lru;   // front = most recent
  struct FailRec { Status st; Ns until; };
  std::map<std::string, FailRec> failed;
  std::vector<std::shared_ptr<VideoSession>> videos;
  FaultHook hook; std::shared_ptr<const DecoderRegistry> registry;
  std::shared_ptr<WorkerCtl> worker; std::vector<std::shared_ptr<WorkerCtl>> orphans;
  std::thread supervisor; std::atomic<bool> shutdown{false};
  struct Cur { bool active = false; Ns start = 0; std::shared_ptr<Ticket> ticket; std::shared_ptr<VideoSession> video; } cur;
  std::shared_ptr<const Frame> placeholder;
  MediaStats st;                                                   // counters under m (cache bytes read from acct)
  int videoLimit = 0; int vw = 0, vh = 0;

  Ns now() const { return clock->nowNs(); }

  // ---- cache admission (caller holds m) ----
  bool reserveLocked(uint64_t b) {
    if (b > budget) return false;
    for (auto it = lru.rbegin(); it != lru.rend() && acct->live.load() + b > budget;) {
      auto ce = cache.find(*it);
      if (ce != cache.end() && ce->second.p.use_count() == 1) {      // only the cache references it -> evictable
        std::string key = *it; auto base = std::next(it).base();
        cache.erase(ce);                                             // drops the payload -> live decreases
        it = std::reverse_iterator<std::list<std::string>::iterator>(lru.erase(base));
        ++st.evictions;
      } else ++it;
    }
    if (acct->live.load() + b > budget) return false;
    acct->add(b); return true;
  }
  void failTicketLocked(const std::shared_ptr<Ticket>& t, Status s) {
    if (t->complete(MediaHandle::State::Failed, s, nullptr, nullptr)) {
      ++st.loadsFailed; failed[t->assetId] = FailRec{s, now() + cfg.failureRetryMs * kNsPerMs};
    }
    auto it = inflight.find(t->assetId); if (it != inflight.end() && it->second == t) inflight.erase(it);
  }
  void startWorkerLocked();
  void recoverLocked(const char* why, bool hung);
};

static Status mkFail(Err c, const std::string& d) { return Status::Fail(c, d, true); }

// ---- loading (worker thread, no locks held) -------------------------------------------------------------------------
static Status loadStillPayload(Impl& I, const AssetSpec& a, std::shared_ptr<Payload>& out) {
  ImportLimits lim = limitsFor(I.cfg.tier);
  DecodedStill d; Status s = validateAndDecodeStillFile(a.path, lim, d);
  if (!s.ok()) return s;
  auto p = std::make_shared<Payload>();
  p->still = frameFromArgb(d.argb.data(), d.w, d.h, I.cfg.matteRgb);
  if (!a.crop.isIdentity()) p->still = cropFrame(p->still, a.crop);
  p->w = p->still.w; p->h = p->still.h; p->bytes = (uint64_t)p->still.px.size() * 4u; p->warnings = d.warnings; p->acct = I.acct;
  out = p; return Status::Ok();
}
static Status loadGifPayload(Impl& I, const AssetSpec& a, std::shared_ptr<Payload>& out) {
  ImportLimits lim = limitsFor(I.cfg.tier);
  if (lim.gifDecodedBudget > I.budget) lim.gifDecodedBudget = I.budget;      // never decode more than we could admit
  DecodedGif g; Status s = validateAndDecodeGifFile(a.path, lim, g);
  if (!s.ok()) return s;
  auto p = std::make_shared<Payload>(); p->anim = true;
  size_t px = (size_t)g.w * g.h;
  p->frames.reserve(g.frameCount);
  for (uint32_t i = 0; i < g.frameCount; ++i) {
    Frame f = frameFromArgb(g.argb.data() + (size_t)i * px, g.w, g.h, I.cfg.matteRgb);
    if (!a.crop.isIdentity()) f = cropFrame(f, a.crop);
    p->bytes += (uint64_t)f.px.size() * 4u; p->frames.push_back(std::move(f));
  }
  p->ends = cumulativeEnds(g.delayMs);
  Ns t0 = 0; for (size_t i = 0; i < p->frames.size(); ++i) { p->frames[i].pts = t0; t0 = p->ends[i]; }
  p->w = p->frames[0].w; p->h = p->frames[0].h; p->warnings = g.warnings; p->acct = I.acct;
  out = p; return Status::Ok();
}

static Status loadVideoSession(Impl& I, const Job& job, std::shared_ptr<VideoSession>& out) {
  const AssetSpec& a = job.spec;
  std::shared_ptr<const DecoderRegistry> reg; { std::lock_guard<std::mutex> lk(I.m); reg = I.registry; }
  std::vector<uint8_t> head; uint64_t fsz = 0;
  Status s = readFileHead(a.path, 2u << 20, head, &fsz); if (!s.ok()) return s;
  AssetKind k; s = checkExtensionAndMagic(a.path, head.data(), head.size(), k); if (!s.ok()) return s;
  if (k != AssetKind::Video) return mkFail(Err::InvalidArgument, "not a video extension");
  Container c = sniffContainer(head.data(), head.size());
  std::string codec = probeVideoCodec(head.data(), head.size(), c), cont = videoContainerKey(a.path);
  if (codec.empty()) return mkFail(Err::UnsupportedCapability, "video codec could not be determined from the container header");
  if (!reg || !reg->isAvailable(cont, codec)) return mkFail(Err::UnsupportedCapability, "no enabled decoder for " + cont + "/" + codec + " in this package");
  std::unique_ptr<IVideoDecoder> dec = reg->create(cont, codec);
  if (!dec) return mkFail(Err::UnsupportedCapability, "decoder factory failed for " + cont + "/" + codec);
  s = dec->open(a.path, I.vw, I.vh);
  if (!s.ok()) { dec->close(); return mkFail(s.code == Err::Ok ? Err::DecodeFailure : s.code, "video open failed: " + baseName(a.path)); }
  auto v = std::make_shared<VideoSession>();
  v->assetId = a.id; v->dec = std::move(dec); v->crop = a.crop; v->loop = a.loop; v->acct = I.acct;
  v->reserved = 3ull * (uint64_t)I.vw * (uint64_t)I.vh * 4u;     // decoder frame + pending + published
  double dur = v->dec->durationSec(); (void)dur;
  out = v; return Status::Ok();
}

// ---- worker ---------------------------------------------------------------------------------------------------------
static void finishJob(const std::shared_ptr<Impl>& I, const std::shared_ptr<WorkerCtl>& me, const Job& job, Status s,
                      std::shared_ptr<Payload> p, std::shared_ptr<VideoSession> v) {
  std::lock_guard<std::mutex> lk(I->m);
  if (me->abandoned.load() || I->shutdown.load()) return;       // result of an abandoned worker is discarded
  I->cur.active = false; I->cur.ticket.reset(); I->cur.video.reset();
  if (!s.ok()) { I->failTicketLocked(job.ticket, s); return; }
  if (job.ticket->done.load()) return;
  if (p) {
    if (!I->reserveLocked(p->bytes)) { I->failTicketLocked(job.ticket, mkFail(Err::ResourceExhausted, "decoded media exceeds cache budget")); return; }
    p->accounted = true;
    I->lru.push_front(job.spec.id);
    I->cache[job.spec.id] = Impl::CacheEntry{p, I->lru.begin()};
    I->failed.erase(job.spec.id);
    job.ticket->complete(MediaHandle::State::Ready, Status::Ok(), p, nullptr);
    ++I->st.loadsOk;
    auto it = I->inflight.find(job.spec.id); if (it != I->inflight.end() && it->second == job.ticket) I->inflight.erase(it);
  } else if (v) {
    if (job.ticket->handles.load() == 0) {                       // every requester already gave up
      try { v->dec->close(); } catch (...) {}
      v->reserved = 0; auto it = I->inflight.find(job.spec.id); if (it != I->inflight.end() && it->second == job.ticket) I->inflight.erase(it);
      job.ticket->complete(MediaHandle::State::Failed, mkFail(Err::Cancelled, "released before ready"), nullptr, nullptr); return;
    }
    if (!I->reserveLocked(v->reserved)) {
      v->reserved = 0; try { v->dec->close(); } catch (...) {}
      I->failTicketLocked(job.ticket, mkFail(Err::ResourceExhausted, "video buffers exceed cache budget")); return;
    }
    I->videos.push_back(v);
    I->failed.erase(job.spec.id);
    job.ticket->complete(MediaHandle::State::Ready, Status::Ok(), nullptr, v);
    ++I->st.loadsOk;                                            // ticket stays in inflight while the session lives
  }
}

static void raiseFault(FaultKind f) {
  if (f == FaultKind::Throw) throw std::runtime_error("injected decoder fault");
  if (f == FaultKind::BadAlloc) throw std::bad_alloc();
  throw 42;
}
static void hangWait(Impl& I, WorkerCtl& me) {
  std::unique_lock<std::mutex> lk(I.m);
  I.cvHang.wait(lk, [&] { return I.shutdown.load() || me.abandoned.load(); });
}

static void runLoad(const std::shared_ptr<Impl>& I, const std::shared_ptr<WorkerCtl>& me, const Job& job, const FaultHook& hook) {
  Status s; std::shared_ptr<Payload> p; std::shared_ptr<VideoSession> v;
  try {
    FaultKind f = hook ? hook(job.spec.id) : FaultKind::None;
    if (f == FaultKind::WorkerDie) { me->exited = true; return; }   // simulated process death: no result, job stays in flight
    if (f == FaultKind::Hang) { hangWait(*I, *me); if (me->abandoned.load() || I->shutdown.load()) return; }
    if (f == FaultKind::Throw || f == FaultKind::ThrowNonStd || f == FaultKind::BadAlloc) raiseFault(f);
    if (job.kind == AssetKind::Still) s = loadStillPayload(*I, job.spec, p);
    else if (job.kind == AssetKind::Gif) s = loadGifPayload(*I, job.spec, p);
    else s = loadVideoSession(*I, job, v);
  } catch (const std::bad_alloc&) { s = mkFail(Err::ResourceExhausted, "out of memory in media worker"); p.reset(); v.reset(); }
  catch (const std::exception&) { s = mkFail(Err::DecodeFailure, "decoder fault contained by media worker"); p.reset(); v.reset(); }
  catch (...) { s = mkFail(Err::InternalFault, "unknown decoder fault contained by media worker"); p.reset(); v.reset(); }
  finishJob(I, me, job, s, p, v);
}

static void closeSessionLocked(Impl& I, const std::shared_ptr<VideoSession>& v) {
  I.videos.erase(std::remove(I.videos.begin(), I.videos.end(), v), I.videos.end());
  auto it = I.inflight.find(v->assetId);
  if (it != I.inflight.end() && it->second->video == v) I.inflight.erase(it);
}

// One pass over live video sessions: decode forward until the newest frame whose pts <= target (bounded per pass).
static void tickVideos(const std::shared_ptr<Impl>& I, const std::shared_ptr<WorkerCtl>& me) {
  std::vector<std::shared_ptr<VideoSession>> list; FaultHook hook; int maxPer;
  { std::lock_guard<std::mutex> lk(I->m); list = I->videos; hook = I->hook; maxPer = I->cfg.maxVideoFramesPerTick; }
  for (auto& s : list) {
    if (me->abandoned.load() || I->shutdown.load()) return;
    if (s->failed.load() || s->released.load()) {
      { std::lock_guard<std::mutex> lk(I->m); closeSessionLocked(*I, s); }
      try { s->dec->close(); } catch (...) {}
      continue;
    }
    { std::lock_guard<std::mutex> lk(I->m); I->cur.active = true; I->cur.start = I->now(); I->cur.ticket.reset(); I->cur.video = s; }
    Status fail;
    try {
      FaultKind f = hook ? hook(s->assetId) : FaultKind::None;
      if (f == FaultKind::WorkerDie) { me->exited = true; return; }
      if (f == FaultKind::Hang) { hangWait(*I, *me); if (me->abandoned.load() || I->shutdown.load()) return; }
      if (f == FaultKind::Throw || f == FaultKind::ThrowNonStd || f == FaultKind::BadAlloc) raiseFault(f);
      Ns target = s->targetNs.load(); int decoded = 0; bool gotNew = false; Frame latest; Ns latestPts = 0;
      while (decoded < maxPer) {
        if (!s->havePending) {
          Frame tmp; Ns pts = 0;
          if (s->ended || !s->dec->nextFrame(tmp, pts)) {
            if (s->loop.load() && !s->ended) {                                   // EOF (or early end): loop
              Ns dur = (Ns)(s->dec->durationSec() * 1e9), alt = s->lastPts + s->frameDur;
              s->loopBase += dur > alt ? dur : alt;
              Status ss = s->dec->seekStart();
              if (!ss.ok()) { fail = mkFail(Err::DecodeFailure, "video seek failed"); break; }
              s->lastPts = 0;
              if (!s->dec->nextFrame(tmp, pts)) { fail = mkFail(Err::DecodeFailure, "video empty after loop"); break; }
            } else { s->ended = true; break; }
          }
          if (s->lastPts > 0 && pts > s->lastPts) s->frameDur = pts - s->lastPts;
          s->lastPts = pts; s->pending = std::move(tmp); s->pendingPts = s->loopBase + pts; s->havePending = true;
        }
        if (s->pendingPts <= target) { latest = std::move(s->pending); latestPts = s->pendingPts; s->havePending = false; gotNew = true; ++decoded; }
        else break;
      }
      if (fail.ok() && !gotNew) {
        bool none; { std::lock_guard<std::mutex> lk(s->mb); none = !s->latest; }
        if (none && s->havePending) { latest = s->pending; latestPts = s->pendingPts; gotNew = true; }   // first frame ahead of its time
      }
      if (fail.ok() && gotNew) {
        Frame out = s->crop.isIdentity() ? std::move(latest) : cropFrame(latest, s->crop);
        out.pts = latestPts;
        auto sp = std::make_shared<const Frame>(std::move(out));
        std::lock_guard<std::mutex> lk(s->mb); s->latest = sp;
      }
    } catch (const std::bad_alloc&) { fail = mkFail(Err::ResourceExhausted, "out of memory in video decode"); }
    catch (const std::exception&) { fail = mkFail(Err::DecodeFailure, "video decoder fault contained by media worker"); }
    catch (...) { fail = mkFail(Err::InternalFault, "unknown video decoder fault contained by media worker"); }
    if (me->abandoned.load()) return;
    {
      std::lock_guard<std::mutex> lk(I->m); I->cur.active = false; I->cur.video.reset();
      if (!fail.ok()) {
        { std::lock_guard<std::mutex> l2(s->mb); s->failStatus = fail; }
        s->failed = true; ++I->st.loadsFailed;
        I->failed[s->assetId] = Impl::FailRec{fail, I->now() + I->cfg.failureRetryMs * kNsPerMs};
        closeSessionLocked(*I, s);
      }
    }
    if (!fail.ok()) { try { s->dec->close(); } catch (...) {} }
  }
}

static void workerMain(std::shared_ptr<Impl> I, std::shared_ptr<WorkerCtl> me) {
  struct Done { std::shared_ptr<WorkerCtl> w; ~Done() { w->exited = true; } } done{me};
  for (;;) {
    Job job; bool haveJob = false; FaultHook hook;
    {
      std::unique_lock<std::mutex> lk(I->m);
      for (;;) {
        if (I->shutdown.load() || me->abandoned.load()) return;
        if (!I->queue.empty()) {
          job = std::move(I->queue.front()); I->queue.pop_front(); haveJob = true; hook = I->hook;
          I->cur.active = true; I->cur.start = I->now(); I->cur.ticket = job.ticket; I->cur.video.reset();
          break;
        }
        if (!I->videos.empty()) break;
        I->cv.wait(lk);
      }
    }
    if (haveJob) { runLoad(I, me, job, hook); if (me->exited.load()) return; }
    else {
      tickVideos(I, me);
      if (me->exited.load()) return;
      std::unique_lock<std::mutex> lk(I->m);
      I->cv.wait_for(lk, std::chrono::milliseconds(4), [&] { return I->shutdown.load() || me->abandoned.load() || !I->queue.empty(); });
    }
  }
}

void Impl::startWorkerLocked() {
  auto w = std::make_shared<WorkerCtl>(); worker = w;
  w->t = std::thread(workerMain, shared_from_this(), w);
}

// Watchdog action (caller holds m): substitute the in-flight source, abandon the stuck/dead worker, start a new one.
void Impl::recoverLocked(const char* why, bool hung) {
  std::string msg = std::string("media worker ") + why + "; source substituted with placeholder";
  Status s = Status::Fail(Err::InternalFault, msg, true);
  if (cur.active) {
    if (cur.ticket) failTicketLocked(cur.ticket, s);
    else if (cur.video) {
      { std::lock_guard<std::mutex> l2(cur.video->mb); cur.video->failStatus = s; }
      cur.video->failed = true; ++st.loadsFailed;
      failed[cur.video->assetId] = FailRec{s, now() + cfg.failureRetryMs * kNsPerMs};
      closeSessionLocked(*this, cur.video);                      // decoder may be stuck inside close(): never call it here
    }
  }
  cur = Cur();
  worker->abandoned = true; orphans.push_back(worker);
  ++st.workerRestarts; if (hung) ++st.hungJobs; else ++st.workerDeaths;
  startWorkerLocked();
  cvHang.notify_all(); cv.notify_all();
}

static void supervisorMain(std::shared_ptr<Impl> I) {
  for (;;) {
    std::vector<std::shared_ptr<WorkerCtl>> reap;
    {
      std::unique_lock<std::mutex> lk(I->m);
      I->cvSup.wait_for(lk, std::chrono::milliseconds(I->cfg.watchdogPollMs), [&] { return I->shutdown.load(); });
      if (I->shutdown.load()) return;
      auto w = I->worker;
      if (w->exited.load() && !w->abandoned.load()) I->recoverLocked("terminated abnormally", false);
      else if (I->cur.active && (I->now() - I->cur.start) > I->cfg.workerTimeoutMs * kNsPerMs) I->recoverLocked("timed out (hung decoder)", true);
      for (auto it = I->orphans.begin(); it != I->orphans.end();) {
        if ((*it)->exited.load()) { reap.push_back(*it); it = I->orphans.erase(it); } else ++it;
      }
    }
    for (auto& w : reap) if (w->t.joinable()) w->t.join();
  }
}

}  // namespace detail

using namespace detail;

// ---- MediaHandle ------------------------------------------------------------------------------------------------------
MediaHandle::MediaHandle(std::shared_ptr<const Frame> ph, std::shared_ptr<Ticket> t, const AssetSpec& spec)
  : ph_(std::move(ph)), t_(std::move(t)), id_(spec.id), loop_(spec.loop), speed_(spec.speed) { t_->handles.fetch_add(1); }

MediaHandle::~MediaHandle() {
  if (t_->handles.fetch_sub(1) == 1) {
    std::shared_ptr<VideoSession> v; { std::lock_guard<std::mutex> lk(t_->m); v = t_->video; }
    if (v) v->released = true;                    // worker closes the decoder on its own thread
  }
}
MediaHandle::State MediaHandle::state() const {
  std::lock_guard<std::mutex> lk(t_->m);
  if (t_->st == State::Ready && t_->video && t_->video->failed.load()) return State::Failed;
  return t_->st;
}
Status MediaHandle::status() const {
  std::lock_guard<std::mutex> lk(t_->m);
  if (t_->st == State::Ready && t_->video && t_->video->failed.load()) { std::lock_guard<std::mutex> l2(t_->video->mb); return t_->video->failStatus; }
  return t_->status;
}
void MediaHandle::setSpeed(double s) { speed_ = s; }
void MediaHandle::setLoop(bool l) {
  loop_ = l;
  std::shared_ptr<VideoSession> v; { std::lock_guard<std::mutex> lk(t_->m); v = t_->video; }
  if (v) v->loop = l;
}

std::shared_ptr<const Frame> MediaHandle::frame(Ns elapsedNs) {
  State st; std::shared_ptr<const Payload> p; std::shared_ptr<VideoSession> v;
  { std::lock_guard<std::mutex> lk(t_->m); st = t_->st; p = t_->payload; v = t_->video; }
  if (st == State::Pending) return nullptr;
  if (st == State::Failed) return ph_;
  auto advance = [&](double lo, double hi) {
    double sp = clampSpeed(speed_, lo, hi);
    if (!started_ || elapsedNs < lastElapsed_) { started_ = true; mediaNs_ = (double)elapsedNs * sp; }
    else mediaNs_ += (double)(elapsedNs - lastElapsed_) * sp;
    lastElapsed_ = elapsedNs; return (Ns)mediaNs_;
  };
  if (p) {
    if (!p->anim) return std::shared_ptr<const Frame>(p, &p->still);
    size_t i = animFrameIndex(p->ends, advance(kGifSpeedMin, kGifSpeedMax), loop_);
    if (i >= p->frames.size()) i = p->frames.size() - 1;
    return std::shared_ptr<const Frame>(p, &p->frames[i]);
  }
  if (v) {
    if (v->failed.load()) return ph_;
    v->targetNs.store(advance(kVideoSpeedMin, kVideoSpeedMax));
    std::lock_guard<std::mutex> lk(v->mb); return v->latest;
  }
  return ph_;
}
int MediaHandle::width() const {
  std::lock_guard<std::mutex> lk(t_->m);
  if (t_->payload) return t_->payload->w;
  if (t_->video) { std::lock_guard<std::mutex> l2(t_->video->mb); return t_->video->latest ? t_->video->latest->w : 0; }
  return 0;
}
int MediaHandle::height() const {
  std::lock_guard<std::mutex> lk(t_->m);
  if (t_->payload) return t_->payload->h;
  if (t_->video) { std::lock_guard<std::mutex> l2(t_->video->mb); return t_->video->latest ? t_->video->latest->h : 0; }
  return 0;
}
std::vector<std::string> MediaHandle::warnings() const {
  std::lock_guard<std::mutex> lk(t_->m); return t_->payload ? t_->payload->warnings : std::vector<std::string>();
}
size_t MediaHandle::frameCount() const {
  std::lock_guard<std::mutex> lk(t_->m);
  return t_->payload ? (t_->payload->anim ? t_->payload->frames.size() : 1) : 0;
}

// ---- MediaManager -----------------------------------------------------------------------------------------------------
MediaManager::MediaManager(const MediaConfig& cfg) : cfg_(cfg) {
  impl_ = std::make_shared<Impl>();
  Impl& I = *impl_;
  if (!cfg_.clock) cfg_.clock = &systemClock();
  if (cfg_.queueCapacity < 1) cfg_.queueCapacity = 1;
  if (cfg_.watchdogPollMs < 1) cfg_.watchdogPollMs = 1;
  if (cfg_.maxVideoFramesPerTick < 1) cfg_.maxVideoFramesPerTick = 1;
  bool base = cfg_.tier == Tier::Base;
  I.cfg = cfg_; I.clock = cfg_.clock;
  I.budget = cfg_.cacheBudgetBytes ? cfg_.cacheBudgetBytes : (base ? kBaseCacheBudgetBytes : kStandardCacheBudgetBytes);
  I.acct->budget = I.budget;
  I.videoLimit = cfg_.maxVideoSources >= 0 ? cfg_.maxVideoSources : (base ? 0 : 1);
  I.vw = cfg_.videoMaxW > 0 ? cfg_.videoMaxW : (base ? 640 : 800);
  I.vh = cfg_.videoMaxH > 0 ? cfg_.videoMaxH : (base ? 360 : 600);
  I.placeholder = sharedPlaceholder(cfg_.placeholderW, cfg_.placeholderH);
  { std::lock_guard<std::mutex> lk(I.m); I.startWorkerLocked(); }
  I.supervisor = std::thread(supervisorMain, impl_);
}

MediaManager::~MediaManager() {
  Impl& I = *impl_;
  std::vector<std::shared_ptr<WorkerCtl>> all;
  { std::lock_guard<std::mutex> lk(I.m); I.shutdown = true; all = I.orphans; all.push_back(I.worker); }
  I.cv.notify_all(); I.cvHang.notify_all(); I.cvSup.notify_all();
  if (I.supervisor.joinable()) I.supervisor.join();
  for (auto& w : all) {
    for (int i = 0; i < 200 && !w->exited.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!w->t.joinable()) continue;
    if (w->exited.load()) w->t.join(); else w->t.detach();      // non-cooperative hang: orphan it (documented limitation)
  }
}

Status MediaManager::registerAsset(const AssetSpec& spec) {
  if (spec.id.empty() || spec.id.size() > 128) return Status::Fail(Err::InvalidArgument, "asset id must be 1..128 chars");
  Status s = validateLocalPath(spec.path); if (!s.ok()) return s;
  AssetKind k; s = classifyExtension(spec.path, k); if (!s.ok()) return s;
  s = validateCrop(spec.crop); if (!s.ok()) return s;
  AssetSpec a = spec;
  a.speed = k == AssetKind::Video ? clampSpeed(a.speed, kVideoSpeedMin, kVideoSpeedMax) : clampSpeed(a.speed, kGifSpeedMin, kGifSpeedMax);
  std::lock_guard<std::mutex> lk(impl_->m);
  if (impl_->assets.size() >= 4096 && !impl_->assets.count(a.id)) return Status::Fail(Err::ResourceExhausted, "too many registered assets");
  impl_->assets[a.id] = std::make_pair(a, k);
  return Status::Ok();
}

MediaHandlePtr MediaManager::request(const std::string& id) {
  Impl& I = *impl_;
  auto mk = [&](std::shared_ptr<Ticket> t, const AssetSpec& sp) { return MediaHandlePtr(new MediaHandle(I.placeholder, std::move(t), sp)); };
  auto failedHandle = [&](const Status& st, bool negCache) {
    auto t = std::make_shared<Ticket>(); t->assetId = id; t->complete(MediaHandle::State::Failed, st, nullptr, nullptr);
    AssetSpec sp; sp.id = id; (void)negCache; return mk(t, sp);
  };
  std::lock_guard<std::mutex> lk(I.m);
  ++I.st.requests;
  if (I.shutdown.load()) return failedHandle(Status::Fail(Err::Cancelled, "media manager stopped"), false);
  auto ai = I.assets.find(id);
  if (ai == I.assets.end()) return failedHandle(Status::Fail(Err::InvalidArgument, "unknown media asset id"), false);
  const AssetSpec spec = ai->second.first; const AssetKind kind = ai->second.second;
  auto ci = I.cache.find(id);
  if (ci != I.cache.end()) {
    I.lru.splice(I.lru.begin(), I.lru, ci->second.it);
    ++I.st.cacheHits;
    auto t = std::make_shared<Ticket>(); t->assetId = id; t->complete(MediaHandle::State::Ready, Status::Ok(), ci->second.p, nullptr);
    return mk(t, spec);
  }
  auto fi = I.failed.find(id);
  if (fi != I.failed.end()) {
    if (I.now() < fi->second.until) return failedHandle(fi->second.st, false);
    I.failed.erase(fi);
  }
  auto ii = I.inflight.find(id);
  if (ii != I.inflight.end()) return mk(ii->second, spec);
  if (kind == AssetKind::Video) {
    if (I.videoLimit <= 0) return failedHandle(Status::Fail(Err::UnsupportedCapability, "video is not admitted on this tier"), false);
    int active = 0; for (auto& kv : I.inflight) { auto a = I.assets.find(kv.first); if (a != I.assets.end() && a->second.second == AssetKind::Video) ++active; }
    if (active >= I.videoLimit) return failedHandle(Status::Fail(Err::ResourceExhausted, "video source limit reached"), false);
  }
  if (I.queue.size() >= I.cfg.queueCapacity) {
    ++I.st.queueRejects;
    return failedHandle(Status::Fail(Err::ResourceExhausted, "media request queue full"), false);
  }
  auto t = std::make_shared<Ticket>(); t->assetId = id;
  auto h = mk(t, spec);                                           // counts the handle before the job can complete
  I.inflight[id] = t;
  I.queue.push_back(Job{spec, kind, t});
  I.cv.notify_one();
  return h;
}

void MediaManager::invalidate(const std::string& id) {
  std::lock_guard<std::mutex> lk(impl_->m);
  auto ci = impl_->cache.find(id);
  if (ci != impl_->cache.end()) { impl_->lru.erase(ci->second.it); impl_->cache.erase(ci); }
  impl_->failed.erase(id);
}
void MediaManager::setDecoderRegistry(const DecoderRegistry& r) {
  auto snap = std::make_shared<const DecoderRegistry>(r);
  std::lock_guard<std::mutex> lk(impl_->m); impl_->registry = snap;
}
void MediaManager::setFaultHook(FaultHook h) { std::lock_guard<std::mutex> lk(impl_->m); impl_->hook = std::move(h); }

MediaStats MediaManager::stats() const {
  std::lock_guard<std::mutex> lk(impl_->m);
  MediaStats s = impl_->st;
  s.cacheBytes = impl_->acct->live.load(); s.cachePeakBytes = impl_->acct->peak.load(); s.cacheBudget = impl_->budget;
  s.queueDepth = impl_->queue.size(); s.cachedItems = impl_->cache.size(); s.activeVideos = impl_->videos.size();
  return s;
}

}} // namespace ss::media
