// Independent re-parsers used by test_rec_*.cpp (deliberately NOT sharing code with src/recording writers).
#pragma once
#include "core/types.h"
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <cstdint>
namespace reccheck {
using B = std::vector<uint8_t>;
inline uint32_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
inline bool is(const uint8_t* p, const char* t) { return std::memcmp(p, t, 4) == 0; }

// ---------------- WAV ----------------
struct Wav { bool ok = false; std::string err; uint32_t rate = 0, ch = 0, bits = 0, fmtTag = 0; uint64_t frames = 0; std::vector<int32_t> pcm; };
inline Wav parseWav(const B& b, bool wantPcm = true) {
  Wav w;
  if (b.size() < 44 || !is(b.data(), "RIFF") || !is(b.data() + 8, "WAVE")) { w.err = "no RIFF/WAVE"; return w; }
  if (le32(b.data() + 4) != b.size() - 8) { w.err = "RIFF size != file size-8"; return w; }
  size_t p = 12; bool fmt = false;
  while (p + 8 <= b.size()) {
    uint32_t sz = le32(b.data() + p + 4);
    if (is(b.data() + p, "fmt ")) { w.fmtTag = le16(b.data() + p + 8); w.ch = le16(b.data() + p + 10); w.rate = le32(b.data() + p + 12); w.bits = le16(b.data() + p + 22); fmt = true;
      if (le32(b.data() + p + 16) != w.rate * w.ch * w.bits / 8 || le16(b.data() + p + 20) != w.ch * w.bits / 8) { w.err = "bad byte rate/block align"; return w; } }
    else if (is(b.data() + p, "data")) {
      if (!fmt) { w.err = "data before fmt"; return w; }
      if (p + 8 + sz != b.size()) { w.err = "data size mismatches file"; return w; }
      w.frames = sz / (w.ch * w.bits / 8);
      if (wantPcm) { const uint8_t* d = b.data() + p + 8; w.pcm.resize((size_t)w.frames * w.ch);
        for (size_t i = 0; i < w.pcm.size(); ++i) { int32_t v = d[i * 3] | (d[i * 3 + 1] << 8) | (d[i * 3 + 2] << 16); if (v & 0x800000) v |= ~0xFFFFFF; w.pcm[i] = v; } }
      w.ok = true; return w;
    }
    p += 8 + sz + (sz & 1);
  }
  w.err = "no data chunk"; return w;
}
inline int32_t pcm24(float f) { if (!(f == f)) f = 0; if (f > 1) f = 1; if (f < -1) f = -1; float s = f * 8388607.0f; return (int32_t)(s >= 0 ? s + 0.5f : s - 0.5f); }

// ---------------- AVI ----------------
struct Avi {
  bool ok = false; std::string err; uint32_t w = 0, h = 0, fpsNum = 0, fpsDen = 0, rate = 0, ch = 0, bits = 0, streams = 0;
  uint32_t avihTotal = 0, vStart = 0, vLen = 0, aLen = 0;
  uint64_t vChunks = 0, vDup = 0, aFrames = 0, idxCount = 0;
  std::vector<std::vector<uint8_t>> frames; B audio;   // raw MJPEG frames (non-empty), concatenated PCM
  std::vector<int32_t> pcm;
};
inline Avi parseAvi(const B& b) {
  Avi a;
  if (b.size() < 12 || !is(b.data(), "RIFF") || !is(b.data() + 8, "AVI ")) { a.err = "no RIFF AVI"; return a; }
  if (le32(b.data() + 4) != b.size() - 8) { a.err = "RIFF size mismatch"; return a; }
  if (b.size() >= (1ull << 31)) { a.err = ">=2GiB"; return a; }
  size_t p = 12; size_t moviTag = 0, moviEnd = 0; bool vs = false; uint32_t ba = 0;
  while (p + 8 <= b.size()) {
    uint32_t sz = le32(b.data() + p + 4);
    if (p + 8 + sz > b.size()) { a.err = "chunk past EOF"; return a; }
    if (is(b.data() + p, "LIST") && is(b.data() + p + 8, "hdrl")) {
      size_t q = p + 12, qe = p + 8 + sz;
      while (q + 8 <= qe) {
        uint32_t s2 = le32(b.data() + q + 4);
        if (is(b.data() + q, "avih")) { a.avihTotal = le32(b.data() + q + 8 + 16); a.streams = le32(b.data() + q + 8 + 24); a.w = le32(b.data() + q + 8 + 32); a.h = le32(b.data() + q + 8 + 36); }
        else if (is(b.data() + q, "LIST")) {
          size_t r = q + 12, re = q + 8 + s2;
          while (r + 8 <= re) {
            uint32_t s3 = le32(b.data() + r + 4);
            if (is(b.data() + r, "strh")) { vs = is(b.data() + r + 8, "vids");
              if (vs) { if (!is(b.data() + r + 12, "MJPG")) { a.err = "video handler not MJPG"; return a; } a.fpsDen = le32(b.data() + r + 28); a.fpsNum = le32(b.data() + r + 32); a.vStart = le32(b.data() + r + 36); a.vLen = le32(b.data() + r + 40); }
              else a.aLen = le32(b.data() + r + 40); }
            else if (is(b.data() + r, "strf") && !vs) { a.ch = le16(b.data() + r + 10); a.rate = le32(b.data() + r + 12); ba = le16(b.data() + r + 20); a.bits = le16(b.data() + r + 22); }
            r += 8 + s3 + (s3 & 1);
          }
        }
        q += 8 + s2 + (s2 & 1);
      }
    } else if (is(b.data() + p, "LIST") && is(b.data() + p + 8, "movi")) { moviTag = p + 8; moviEnd = p + 8 + sz; }
    else if (is(b.data() + p, "idx1")) {
      if (!moviTag) { a.err = "idx1 before movi"; return a; }
      a.idxCount = sz / 16;
      for (uint32_t i = 0; i < sz / 16; ++i) {
        const uint8_t* e = b.data() + p + 8 + i * 16; size_t off = moviTag + le32(e + 8); uint32_t esz = le32(e + 12);
        if (off + 8 > b.size() || std::memcmp(b.data() + off, e, 4) != 0 || le32(b.data() + off + 4) != esz) { a.err = "idx1 entry does not match chunk"; return a; }
      }
    }
    p += 8 + sz + (sz & 1);
  }
  if (!moviTag) { a.err = "no movi"; return a; }
  // chunk walk
  size_t c = moviTag + 4; uint64_t nchunks = 0;
  while (c + 8 <= moviEnd) {
    uint32_t sz = le32(b.data() + c + 4);
    if (is(b.data() + c, "00dc")) { ++a.vChunks; if (sz == 0) ++a.vDup; else a.frames.emplace_back(b.begin() + c + 8, b.begin() + c + 8 + sz); }
    else if (is(b.data() + c, "01wb")) { if (!ba) { a.err = "audio chunk without audio stream"; return a; } a.aFrames += sz / ba; a.audio.insert(a.audio.end(), b.begin() + c + 8, b.begin() + c + 8 + sz); }
    else { a.err = "unknown chunk in movi"; return a; }
    ++nchunks; c += 8 + sz + (sz & 1);
  }
  if (c != moviEnd) { a.err = "movi size does not match chunk walk"; return a; }
  if (a.idxCount != nchunks) { a.err = "idx1 count != chunks"; return a; }
  if (a.avihTotal != a.vChunks || a.vLen != a.vChunks) { a.err = "frame counts in header differ from chunks"; return a; }
  if (ba && a.aLen != a.aFrames) { a.err = "audio length in header differs"; return a; }
  if (a.bits == 24) { a.pcm.resize(a.audio.size() / 3); for (size_t i = 0; i < a.pcm.size(); ++i) { const uint8_t* d = &a.audio[i * 3]; int32_t v = d[0] | (d[1] << 8) | (d[2] << 16); if (v & 0x800000) v |= ~0xFFFFFF; a.pcm[i] = v; } }
  a.ok = true; return a;
}

// ---------------- Matroska ----------------
struct MkvBlock { int track; int64_t ms; bool key; size_t size; B data; };
struct Mkv {
  bool ok = false; std::string err; std::string docType; bool segUnknown = false;
  struct Track { int num = 0, type = 0; std::string codec; int w = 0, h = 0, ch = 0, bits = 0; double rate = 0; uint64_t defDur = 0; };
  std::vector<Track> tracks; std::vector<MkvBlock> blocks; std::vector<int64_t> clusterMs; std::vector<int64_t> clusterSpan;
  std::vector<std::pair<std::string, std::string>> tags; size_t cues = 0; std::vector<std::pair<int64_t, uint64_t>> cuePts; double durationMs = 0; double timecodeScale = 0;
  size_t clusters = 0; std::vector<uint8_t> unknownSizedClusters; std::map<uint32_t, uint64_t> seek;   // id -> segment-relative pos
  bool seekVerified = false; bool truncated = false; int audioBlockCount = 0, videoBlockCount = 0;
};
struct EbmlReader {
  const B& b; EbmlReader(const B& x) : b(x) {}
  bool id(size_t& p, uint32_t& v) const { if (p >= b.size()) return false; uint8_t f = b[p]; int len = f >= 0x80 ? 1 : f >= 0x40 ? 2 : f >= 0x20 ? 3 : f >= 0x10 ? 4 : 0; if (!len || p + len > b.size()) return false; v = 0; for (int i = 0; i < len; ++i) v = (v << 8) | b[p + i]; p += len; return true; }
  bool size(size_t& p, uint64_t& v, bool& unknown) const {
    if (p >= b.size()) return false;
    uint8_t f = b[p]; int len = 1; while (len <= 8 && !(f & (0x80 >> (len - 1)))) ++len; if (len > 8 || p + len > b.size()) return false;
    v = f & (0xFF >> len); bool allOnes = v == (uint64_t)(0xFF >> len); for (int i = 1; i < len; ++i) { v = (v << 8) | b[p + i]; if (b[p + i] != 0xFF) allOnes = false; }
    unknown = allOnes; p += len; return true; }
  uint64_t uintv(size_t p, size_t n) const { uint64_t v = 0; for (size_t i = 0; i < n; ++i) v = (v << 8) | b[p + i]; return v; }
  double flt(size_t p, size_t n) const { if (n == 4) { uint32_t u = (uint32_t)uintv(p, 4); float f; std::memcpy(&f, &u, 4); return f; } uint64_t u = uintv(p, 8); double d; std::memcpy(&d, &u, 8); return d; }
};
inline Mkv parseMkv(const B& b) {
  Mkv m; EbmlReader r(b); size_t p = 0; uint32_t id; uint64_t sz; bool unk;
  if (!r.id(p, id) || id != 0x1A45DFA3 || !r.size(p, sz, unk) || unk) { m.err = "no EBML header"; return m; }
  size_t hend = p + sz;
  while (p < hend) { uint32_t i2; uint64_t s2; bool u2; if (!r.id(p, i2) || !r.size(p, s2, u2)) { m.err = "bad hdr"; return m; } if (i2 == 0x4282) m.docType.assign((const char*)&b[p], s2); p += s2; }
  if (m.docType != "matroska") { m.err = "docType"; return m; }
  if (!r.id(p, id) || id != 0x18538067 || !r.size(p, sz, unk)) { m.err = "no Segment"; return m; }
  m.segUnknown = unk; size_t segData = p; size_t segEnd = unk ? b.size() : p + sz;
  if (segEnd > b.size()) { m.err = "segment size beyond file"; return m; }
  m.timecodeScale = 1000000;
  struct Pend { uint32_t id; size_t pos; };
  std::vector<Pend> top;
  while (p < segEnd) {
    size_t elStart = p; uint32_t eid; uint64_t esz; bool eu;
    if (!r.id(p, eid) || !r.size(p, esz, eu)) { m.err = "bad element header in segment"; return m; }
    top.push_back({eid, elStart - segData});
    size_t cend = eu ? segEnd : p + esz;
    if (cend > segEnd) { m.err = "child exceeds segment"; return m; }
    if (eid == 0x114D9B74) {
      size_t q = p; while (q < cend) { uint32_t si; uint64_t ss; bool su; r.id(q, si); r.size(q, ss, su); size_t se = q + ss; uint32_t sid = 0; uint64_t spos = 0;
        size_t k = q; while (k < se) { uint32_t ci; uint64_t cs; bool cu; r.id(k, ci); r.size(k, cs, cu); if (ci == 0x53AB) { size_t kk = k; uint32_t t; r.id(kk, t); sid = t; } else if (ci == 0x53AC) spos = r.uintv(k, cs); k += cs; }
        m.seek[sid] = spos; q = se; }
    } else if (eid == 0x1549A966) {
      size_t q = p; while (q < cend) { uint32_t ci; uint64_t cs; bool cu; r.id(q, ci); r.size(q, cs, cu); if (ci == 0x4489) m.durationMs = r.flt(q, cs); else if (ci == 0x2AD7B1) m.timecodeScale = (double)r.uintv(q, cs); q += cs; }
    } else if (eid == 0x1654AE6B) {
      size_t q = p; while (q < cend) { uint32_t ci; uint64_t cs; bool cu; r.id(q, ci); r.size(q, cs, cu);
        if (ci == 0xAE) { Mkv::Track t; size_t k = q, ke = q + cs; while (k < ke) { uint32_t ti; uint64_t ts; bool tu; r.id(k, ti); r.size(k, ts, tu);
            if (ti == 0xD7) t.num = (int)r.uintv(k, ts); else if (ti == 0x83) t.type = (int)r.uintv(k, ts); else if (ti == 0x86) t.codec.assign((const char*)&b[k], ts); else if (ti == 0x23E383) t.defDur = r.uintv(k, ts);
            else if (ti == 0xE0 || ti == 0xE1) { size_t v = k, ve = k + ts; while (v < ve) { uint32_t vi; uint64_t vs; bool vu; r.id(v, vi); r.size(v, vs, vu);
                if (vi == 0xB0) t.w = (int)r.uintv(v, vs); else if (vi == 0xBA) t.h = (int)r.uintv(v, vs); else if (vi == 0xB5) t.rate = r.flt(v, vs); else if (vi == 0x9F) t.ch = (int)r.uintv(v, vs); else if (vi == 0x6264) t.bits = (int)r.uintv(v, vs); v += vs; } }
            k += ts; } m.tracks.push_back(t); }
        q += cs; }
    } else if (eid == 0x1F43B675) {
      ++m.clusters; if (eu) m.unknownSizedClusters.push_back(1);
      int64_t cms = 0; size_t q = p; int64_t mn = INT64_MAX, mx = INT64_MIN;
      while (q < cend) {
        size_t qs = q; uint32_t ci; uint64_t cs; bool cu; if (!r.id(q, ci) || !r.size(q, cs, cu)) { if (eu) break; m.err = "bad cluster child"; return m; }
        if (eu && (ci == 0x1F43B675 || ci == 0x1C53BB6B || ci == 0x1254C367)) { q = qs; break; }   // unknown-size cluster ends at next top-level
        if (ci == 0xE7) cms = (int64_t)r.uintv(q, cs);
        else if (ci == 0xA3) { if (q + cs > b.size()) { m.truncated = true; q = cend; break; } if (cs < 4) { m.err = "bad SimpleBlock"; return m; } MkvBlock bl; bl.track = b[q] & 0x7F; int16_t rel = (int16_t)((b[q + 1] << 8) | b[q + 2]); bl.key = b[q + 3] & 0x80; bl.ms = cms + rel; bl.size = cs - 4; bl.data.assign(b.begin() + q + 4, b.begin() + q + cs); m.blocks.push_back(bl); mn = std::min(mn, bl.ms); mx = std::max(mx, bl.ms); }
        q += cs;
      }
      m.clusterMs.push_back(cms); m.clusterSpan.push_back(mx >= mn ? mx - cms : 0);
      if (eu) { p = q; continue; }
    } else if (eid == 0x1254C367) {
      size_t q = p; while (q < cend) { uint32_t ci; uint64_t cs; bool cu; r.id(q, ci); r.size(q, cs, cu); if (ci == 0x7373) { size_t k = q, ke = q + cs; while (k < ke) { uint32_t ti; uint64_t ts; bool tu; r.id(k, ti); r.size(k, ts, tu); if (ti == 0x67C8) { size_t v = k, ve = k + ts; std::string n, s; while (v < ve) { uint32_t vi; uint64_t vs; bool vu; r.id(v, vi); r.size(v, vs, vu); if (vi == 0x45A3) n.assign((const char*)&b[v], vs); else if (vi == 0x4487) s.assign((const char*)&b[v], vs); v += vs; } m.tags.emplace_back(n, s); } k += ts; } } q += cs; }
    } else if (eid == 0x1C53BB6B) {
      size_t q = p; while (q < cend) { uint32_t ci; uint64_t cs; bool cu; r.id(q, ci); r.size(q, cs, cu); if (ci == 0xBB) { ++m.cues; int64_t t = 0; uint64_t cp = 0; size_t k = q, ke = q + cs; while (k < ke) { uint32_t ti; uint64_t ts; bool tu; r.id(k, ti); r.size(k, ts, tu); if (ti == 0xB3) t = (int64_t)r.uintv(k, ts); else if (ti == 0xB7) { size_t v = k, ve = k + ts; while (v < ve) { uint32_t vi; uint64_t vs; bool vu; r.id(v, vi); r.size(v, vs, vu); if (vi == 0xF1) cp = r.uintv(v, vs); v += vs; } } k += ts; } m.cuePts.push_back({t, cp}); } q += cs; }
    }
    p = cend;
  }
  // verify seek entries point at the right element IDs; cues point at clusters
  m.seekVerified = true;
  for (auto& kv : m.seek) { bool f = false; for (auto& t : top) if (t.pos == kv.second && t.id == kv.first) f = true; if (!f) m.seekVerified = false; }
  for (auto& c : m.cuePts) { bool f = false; for (auto& t : top) if (t.pos == c.second && t.id == 0x1F43B675) f = true; if (!f) m.seekVerified = false; }
  for (auto& bl : m.blocks) { if (bl.track == 1) ++m.videoBlockCount; else if (bl.track == 2) ++m.audioBlockCount; }
  m.ok = true; return m;
}
inline bool isJpeg(const B& d) { return d.size() > 4 && d[0] == 0xFF && d[1] == 0xD8 && d[d.size() - 2] == 0xFF && d[d.size() - 1] == 0xD9; }
inline std::vector<int32_t> pcm24le(const B& d) { std::vector<int32_t> v(d.size() / 3); for (size_t i = 0; i < v.size(); ++i) { int32_t x = d[i * 3] | (d[i * 3 + 1] << 8) | (d[i * 3 + 2] << 16); if (x & 0x800000) x |= ~0xFFFFFF; v[i] = x; } return v; }
} // namespace reccheck
