// WinMM MIDI 1.0 port (MOD-09, REQ-M04). Driver callbacks only enqueue; a worker thread delivers, re-arms SysEx buffers,
// and polls for device loss (<= 250 ms) so the lost callback fires well inside the 1 s budget.
#include "win_internal.h"
#include <mmsystem.h>
#include <atomic>
#include <cstring>
#include <map>
#include <algorithm>
#include <thread>
#include <condition_variable>

namespace ss { namespace win {
using namespace detail;
namespace {

struct DevKey { std::string stable, name; UINT index; };

std::vector<DevKey> listIn() {
  std::vector<DevKey> out; std::map<std::string, int> seen;
  UINT n = midiInGetNumDevs();
  for (UINT i = 0; i < n; ++i) {
    MIDIINCAPSW c; ZeroMemory(&c, sizeof c);
    if (midiInGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
    std::string name = narrow(c.szPname, wcsnlen(c.szPname, 32));
    std::string base = "in:" + name + "|" + hex(c.wMid, 4) + "." + hex(c.wPid, 4);
    out.push_back({base + "#" + std::to_string(seen[base]++), name, i});
  }
  return out;
}
std::vector<DevKey> listOut() {
  std::vector<DevKey> out; std::map<std::string, int> seen;
  UINT n = midiOutGetNumDevs();
  for (UINT i = 0; i < n; ++i) {
    MIDIOUTCAPSW c; ZeroMemory(&c, sizeof c);
    if (midiOutGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
    std::string name = narrow(c.szPname, wcsnlen(c.szPname, 32));
    std::string base = "out:" + name + "|" + hex(c.wMid, 4) + "." + hex(c.wPid, 4);
    out.push_back({base + "#" + std::to_string(seen[base]++), name, i});
  }
  return out;
}

int shortLen(uint8_t st) {
  if (st >= 0xF8) return 1;
  switch (st & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 3;
    case 0xC0: case 0xD0: return 2;
    default: break;
  }
  switch (st) { case 0xF1: case 0xF3: return 2; case 0xF2: return 3; case 0xF6: return 1; default: return 0; }
}

constexpr int kQ = 512, kSysexBufs = 4, kSysexSize = 4096;

struct Ev { Ns t; uint8_t len; uint8_t b[3]; int sysex; /* >=0: buffer index */ uint32_t sysexLen; };

class WinmmMidi : public IMidiPort {
 public:
  WinmmMidi() { InitializeCriticalSection(&qcs_); }
  ~WinmmMidi() override { closeInput(); closeOutput(); stopWorker(); DeleteCriticalSection(&qcs_); }

  std::vector<MidiPortInfo> enumerate() override {
    std::vector<MidiPortInfo> r;
    for (auto& d : listIn()) r.push_back({d.stable, d.name, true});
    for (auto& d : listOut()) r.push_back({d.stable, d.name, false});
    return r;
  }

  Status openInput(const std::string& id, std::function<void(Ns, const uint8_t*, size_t)> cb) override {
    closeInput();
    const DevKey* sel = nullptr; auto devs = listIn();
    for (auto& d : devs) if (d.stable == id) { sel = &d; break; }
    if (!sel) return Status::Fail(Err::DeviceLost, "MIDI input not present");
    startWorker();
    std::lock_guard<std::mutex> dl(devM_);
    inCb_ = std::make_shared<std::function<void(Ns, const uint8_t*, size_t)>>(std::move(cb));
    inKey_ = sel->stable; head_ = tail_ = count_ = 0; dropped_ = 0;
    MMRESULT mr = midiInOpen(&hin_, sel->index, (DWORD_PTR)&WinmmMidi::inProc, (DWORD_PTR)this, CALLBACK_FUNCTION);
    if (mr != MMSYSERR_NOERROR) { hin_ = nullptr; return mmError(mr, "midiInOpen"); }
    for (int i = 0; i < kSysexBufs; ++i) {
      sx_[i].assign(kSysexSize, 0);
      ZeroMemory(&sxh_[i], sizeof(MIDIHDR));
      sxh_[i].lpData = (LPSTR)sx_[i].data(); sxh_[i].dwBufferLength = kSysexSize;
      midiInPrepareHeader(hin_, &sxh_[i], sizeof(MIDIHDR));
      midiInAddBuffer(hin_, &sxh_[i], sizeof(MIDIHDR));
    }
    inLost_ = false; inAlive_ = true; closingIn_ = false;
    midiInStart(hin_);
    return Status::Ok();
  }

  Status openOutput(const std::string& id) override {
    closeOutput();
    const DevKey* sel = nullptr; auto devs = listOut();
    for (auto& d : devs) if (d.stable == id) { sel = &d; break; }
    if (!sel) return Status::Fail(Err::DeviceLost, "MIDI output not present");
    startWorker();
    std::lock_guard<std::mutex> dl(devM_);
    MMRESULT mr = midiOutOpen(&hout_, sel->index, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) { hout_ = nullptr; return mmError(mr, "midiOutOpen"); }
    outKey_ = sel->stable; outLost_ = false; outAlive_ = true;
    return Status::Ok();
  }

  Status send(const uint8_t* d, size_t n) override {
    if (!d || !n) return Status::Fail(Err::InvalidArgument, "empty MIDI message");
    std::lock_guard<std::mutex> dl(devM_);
    if (!hout_ || !outAlive_) return Status::Fail(Err::DeviceLost, "MIDI output not open");
    size_t i = 0;
    while (i < n) {
      uint8_t st = d[i];
      if (st == 0xF0) {
        size_t e = i; while (e < n && d[e] != 0xF7) ++e;
        if (e >= n) return Status::Fail(Err::InvalidArgument, "unterminated SysEx");
        size_t len = e - i + 1;
        if (len > 65536) return Status::Fail(Err::InvalidArgument, "SysEx too large");
        std::vector<uint8_t> copy(d + i, d + i + len);
        MIDIHDR h; ZeroMemory(&h, sizeof h); h.lpData = (LPSTR)copy.data(); h.dwBufferLength = (DWORD)len;
        MMRESULT mr = midiOutPrepareHeader(hout_, &h, sizeof h);
        if (mr == MMSYSERR_NOERROR) {
          mr = midiOutLongMsg(hout_, &h, sizeof h);
          if (mr == MMSYSERR_NOERROR) {
            for (int w = 0; w < 1000 && !(h.dwFlags & MHDR_DONE); ++w) Sleep(1);   // worker thread only; bounded 1 s
            if (!(h.dwFlags & MHDR_DONE)) { markOutLost(); return Status::Fail(Err::DeviceLost, "SysEx transmit timed out"); }
          }
          midiOutUnprepareHeader(hout_, &h, sizeof h);
        }
        if (mr != MMSYSERR_NOERROR) return outError(mr);
        i = e + 1;
        continue;
      }
      if (st < 0x80) return Status::Fail(Err::InvalidArgument, "MIDI data byte without status (running status not accepted)");
      int len = shortLen(st);
      if (len == 0) return Status::Fail(Err::InvalidArgument, "unsupported MIDI status byte");
      if (i + (size_t)len > n) return Status::Fail(Err::InvalidArgument, "truncated MIDI message");
      DWORD msg = st; if (len > 1) msg |= (DWORD)d[i + 1] << 8; if (len > 2) msg |= (DWORD)d[i + 2] << 16;
      MMRESULT mr = midiOutShortMsg(hout_, msg);
      if (mr != MMSYSERR_NOERROR) return outError(mr);
      i += (size_t)len;
    }
    return Status::Ok();
  }

  void closeInput() override {
    std::lock_guard<std::mutex> dl(devM_);
    if (!hin_) return;
    closingIn_ = true;
    midiInStop(hin_); midiInReset(hin_);
    for (int i = 0; i < kSysexBufs; ++i) midiInUnprepareHeader(hin_, &sxh_[i], sizeof(MIDIHDR));
    midiInClose(hin_); hin_ = nullptr; inAlive_ = false;
    EnterCriticalSection(&qcs_); head_ = tail_ = count_ = 0; LeaveCriticalSection(&qcs_);   // no stale events after reconnect (REQ-M04)
    inCb_.reset();
  }
  void closeOutput() override {
    std::lock_guard<std::mutex> dl(devM_);
    if (!hout_) return;
    midiOutReset(hout_); midiOutClose(hout_); hout_ = nullptr; outAlive_ = false;
  }
  bool inputAlive() const override { return inAlive_; }
  bool outputAlive() const override { return outAlive_; }
  void setLostCallback(std::function<void(bool)> cb) override { std::lock_guard<std::mutex> l(cbM_); lostCb_ = cb ? std::make_shared<std::function<void(bool)>>(std::move(cb)) : nullptr; }

 private:
  static Status mmError(MMRESULT mr, const char* what) {
    if (mr == MMSYSERR_ALLOCATED) return Status::Fail(Err::DeviceLost, std::string(what) + ": port is in use by another application");
    if (mr == MMSYSERR_BADDEVICEID || mr == MMSYSERR_NODRIVER) return Status::Fail(Err::DeviceLost, std::string(what) + ": device not available");
    return Status::Fail(Err::InternalFault, std::string(what) + " failed (MMRESULT " + std::to_string(mr) + ")");
  }
  Status outError(MMRESULT mr) {
    if (mr == MMSYSERR_NODRIVER || mr == MMSYSERR_INVALHANDLE || mr == MMSYSERR_BADDEVICEID || mr == MIDIERR_NODEVICE) { markOutLost(); return Status::Fail(Err::DeviceLost, "MIDI output lost"); }
    return Status::Fail(Err::TransportFailure, "MIDI send failed (MMRESULT " + std::to_string(mr) + ")");
  }
  void markOutLost() { outAlive_ = false; SetEvent(wakeEv_); }

  // ---- driver callback: enqueue only (no midiIn* calls here)
  static void CALLBACK inProc(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
    WinmmMidi* self = reinterpret_cast<WinmmMidi*>(inst);
    if (!self || self->closingIn_) return;
    if (msg == MIM_DATA) {
      uint8_t st = (uint8_t)(p1 & 0xFF);
      if (st < 0x80) return;
      int len = shortLen(st); if (!len) return;
      Ev e; e.t = qpcNowNs(); e.len = (uint8_t)len; e.b[0] = st; e.b[1] = (uint8_t)((p1 >> 8) & 0xFF); e.b[2] = (uint8_t)((p1 >> 16) & 0xFF); e.sysex = -1; e.sysexLen = 0;
      self->push(e);
    } else if (msg == MIM_LONGDATA) {
      MIDIHDR* h = reinterpret_cast<MIDIHDR*>(p1);
      int idx = -1; for (int i = 0; i < kSysexBufs; ++i) if (&self->sxh_[i] == h) idx = i;
      if (idx < 0) return;
      Ev e; e.t = qpcNowNs(); e.len = 0; e.sysex = idx; e.sysexLen = h->dwBytesRecorded; e.b[0] = e.b[1] = e.b[2] = 0;
      self->push(e);
    } else if (msg == MIM_ERROR || msg == MIM_LONGERROR) {
      // invalid data from driver: ignored (counted as dropped)
      ++self->dropped_;
    }
    SetEvent(self->wakeEv_);
  }
  void push(const Ev& e) {
    EnterCriticalSection(&qcs_);
    if (count_ == kQ) {                       // full: drop oldest continuous controller first, else oldest short message
      int drop = -1;
      for (int k = 0; k < count_ && drop < 0; ++k) { int ix = (head_ + k) % kQ; if (q_[ix].sysex < 0 && (q_[ix].b[0] & 0xF0) == 0xB0) drop = ix; }
      for (int k = 0; k < count_ && drop < 0; ++k) { int ix = (head_ + k) % kQ; if (q_[ix].sysex < 0) drop = ix; }
      if (drop < 0) { ++dropped_; LeaveCriticalSection(&qcs_); return; }   // never drop SysEx buffer completions (re-arm needed)
      int last = (head_ + count_ - 1) % kQ;
      for (int ix = drop; ix != last; ix = (ix + 1) % kQ) q_[ix] = q_[(ix + 1) % kQ];
      --count_; tail_ = (head_ + count_) % kQ; ++dropped_;
    }
    q_[tail_] = e; tail_ = (tail_ + 1) % kQ; ++count_;
    LeaveCriticalSection(&qcs_);
  }
  bool pop(Ev& e) {
    EnterCriticalSection(&qcs_);
    bool ok = count_ > 0;
    if (ok) { e = q_[head_]; head_ = (head_ + 1) % kQ; --count_; }
    LeaveCriticalSection(&qcs_);
    return ok;
  }

  void startWorker() {
    std::lock_guard<std::mutex> l(wm_);
    if (th_.joinable()) return;
    wakeEv_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopW_ = false;
    th_ = std::thread([this] { workerLoop(); });
  }
  void stopWorker() {
    std::lock_guard<std::mutex> l(wm_);
    if (th_.joinable()) { stopW_ = true; SetEvent(wakeEv_); th_.join(); }
    if (wakeEv_) { CloseHandle(wakeEv_); wakeEv_ = nullptr; }
  }

  void fireLost(bool input) {
    std::shared_ptr<std::function<void(bool)>> cb; { std::lock_guard<std::mutex> l(cbM_); cb = lostCb_; }
    if (cb) (*cb)(input);
  }

  void workerLoop() {
    while (!stopW_) {
      WaitForSingleObject(wakeEv_, 100);
      if (stopW_) break;
      // deliver
      std::shared_ptr<std::function<void(Ns, const uint8_t*, size_t)>> cb;
      { std::lock_guard<std::mutex> dl(devM_); cb = inCb_; }
      Ev e;
      while (pop(e)) {
        if (e.sysex >= 0) {
          std::vector<uint8_t> copy;
          {
            std::lock_guard<std::mutex> dl(devM_);
            if (hin_ && !closingIn_) {
              copy.assign(sx_[e.sysex].begin(), sx_[e.sysex].begin() + std::min<size_t>(e.sysexLen, kSysexSize));
              sxh_[e.sysex].dwBytesRecorded = 0;
              midiInAddBuffer(hin_, &sxh_[e.sysex], sizeof(MIDIHDR));
            }
          }
          if (cb && !copy.empty()) (*cb)(e.t, copy.data(), copy.size());
        } else if (cb) {
          (*cb)(e.t, e.b, e.len);
        }
      }
      // loss detection (poll)
      bool inLostNow = false, outLostNow = false;
      {
        std::lock_guard<std::mutex> dl(devM_);
        if (hin_ && inAlive_) {
          UINT id = 0;
          if (midiInGetID(hin_, &id) != MMSYSERR_NOERROR || !present(listIn(), inKey_)) { inAlive_ = false; inLostNow = !inLost_; inLost_ = true; }
        }
        if (hout_ && !outAlive_ && !outLost_) { outLostNow = true; outLost_ = true; }
        else if (hout_ && outAlive_) {
          UINT id = 0;
          if (midiOutGetID(hout_, &id) != MMSYSERR_NOERROR || !present(listOut(), outKey_)) { outAlive_ = false; outLostNow = !outLost_; outLost_ = true; }
        }
      }
      if (inLostNow) fireLost(true);
      if (outLostNow) fireLost(false);
    }
  }
  static bool present(const std::vector<DevKey>& l, const std::string& k) { for (auto& d : l) if (d.stable == k) return true; return false; }

  CRITICAL_SECTION qcs_; Ev q_[kQ]; int head_ = 0, tail_ = 0, count_ = 0; uint64_t dropped_ = 0;
  std::mutex devM_, wm_, cbM_;
  HMIDIIN hin_ = nullptr; HMIDIOUT hout_ = nullptr;
  MIDIHDR sxh_[kSysexBufs]; std::vector<uint8_t> sx_[kSysexBufs];
  std::shared_ptr<std::function<void(Ns, const uint8_t*, size_t)>> inCb_;
  std::shared_ptr<std::function<void(bool)>> lostCb_;
  std::string inKey_, outKey_;
  std::atomic<bool> inAlive_{false}, outAlive_{false}, closingIn_{false}, stopW_{false};
  bool inLost_ = false, outLost_ = false;
  std::thread th_; HANDLE wakeEv_ = nullptr;
};

} // namespace
std::unique_ptr<IMidiPort> makeMidiPort() { return std::unique_ptr<IMidiPort>(new WinmmMidi()); }
}} // namespace
