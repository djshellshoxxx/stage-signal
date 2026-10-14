#include "persistence/inflate.h"
#include <cstring>

namespace ss {
namespace persist {
namespace {

struct Huff { uint16_t count[16]; uint16_t symbol[288]; };

struct Dec {
  InflateIO& io; uint64_t maxOut; uint64_t produced = 0;
  uint8_t inbuf[4096]; size_t inLen = 0, inPos = 0; bool inEof = false;
  uint32_t bitBuf = 0; int bitCnt = 0;
  uint8_t win[32768]; size_t wpos = 0; bool failed = false; Err ferr = Err::DecodeFailure; const char* fmsg = "";
  Dec(InflateIO& i, uint64_t m) : io(i), maxOut(m) {}

  void fail(Err e, const char* m) { if (!failed) { failed = true; ferr = e; fmsg = m; } }
  int byte() {
    if (inPos >= inLen) {
      if (inEof) return -1;
      inLen = io.in(inbuf, sizeof(inbuf)); inPos = 0;
      if (inLen == 0) { inEof = true; return -1; }
    }
    return inbuf[inPos++];
  }
  uint32_t bits(int need) {
    while (bitCnt < need) {
      int b = byte();
      if (b < 0) { fail(Err::DecodeFailure, "truncated deflate stream"); return 0; }
      bitBuf |= (uint32_t)b << bitCnt; bitCnt += 8;
    }
    uint32_t v = bitBuf & ((1u << need) - 1);
    bitBuf >>= need; bitCnt -= need;
    return v;
  }
  bool flushWin(size_t n) {
    if (n && !io.out(win, n)) { fail(Err::InternalFault, "output sink failed"); return false; }
    return true;
  }
  bool emit(uint8_t b) {
    if (produced >= maxOut) { fail(Err::ResourceExhausted, "decompressed size exceeds limit"); return false; }
    win[wpos++] = b; ++produced;
    if (wpos == sizeof(win)) { if (!flushWin(wpos)) return false; wpos = 0; }
    return true;
  }
  int decode(const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
      code |= (int)bits(1);
      if (failed) return -1;
      int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count; first += count; first <<= 1; code <<= 1;
    }
    fail(Err::DecodeFailure, "bad huffman code");
    return -1;
  }
  static int build(Huff& h, const uint16_t* length, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; ++i) h.count[length[i]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; ++len) { left <<= 1; left -= h.count[len]; if (left < 0) return left; }
    uint16_t offs[16]; offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = (uint16_t)(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i) if (length[i] != 0) h.symbol[offs[length[i]]++] = (uint16_t)i;
    return left;
  }
  bool codes(const Huff& lc, const Huff& dc) {
    static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint16_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint16_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
      int sym = decode(lc);
      if (failed) return false;
      if (sym < 256) { if (!emit((uint8_t)sym)) return false; }
      else if (sym == 256) return true;
      else {
        sym -= 257;
        if (sym >= 29) { fail(Err::DecodeFailure, "bad length symbol"); return false; }
        int len = lbase[sym] + (int)bits(lext[sym]);
        int ds = decode(dc);
        if (failed) return false;
        if (ds < 0 || ds >= 30) { fail(Err::DecodeFailure, "bad distance symbol"); return false; }
        uint32_t dist = dbase[ds] + bits(dext[ds]);
        if (failed) return false;
        if (dist > produced) { fail(Err::DecodeFailure, "distance too far back"); return false; }
        for (int k = 0; k < len; ++k) {
          // window holds the most recent bytes in a ring; wpos is the next write slot
          uint8_t b = win[(wpos + sizeof(win) - dist) % sizeof(win)];
          if (!emit(b)) return false;
        }
      }
    }
  }
  bool stored() {
    bitBuf = 0; bitCnt = 0;
    int a = byte(), b = byte(), c = byte(), d = byte();
    if (a < 0 || b < 0 || c < 0 || d < 0) { fail(Err::DecodeFailure, "truncated stored block"); return false; }
    unsigned len = (unsigned)(a | (b << 8)), nlen = (unsigned)(c | (d << 8));
    if (len != (~nlen & 0xFFFF)) { fail(Err::DecodeFailure, "stored block length mismatch"); return false; }
    for (unsigned i = 0; i < len; ++i) { int x = byte(); if (x < 0) { fail(Err::DecodeFailure, "truncated stored block"); return false; } if (!emit((uint8_t)x)) return false; }
    return true;
  }
  struct FixedTables { Huff lc, dc; FixedTables() {
      uint16_t l[288]; int s = 0;
      for (; s < 144; ++s) l[s] = 8;
      for (; s < 256; ++s) l[s] = 9;
      for (; s < 280; ++s) l[s] = 7;
      for (; s < 288; ++s) l[s] = 8;
      build(lc, l, 288);
      for (s = 0; s < 30; ++s) l[s] = 5;
      build(dc, l, 30);
  } };
  bool fixed() {
    static const FixedTables t;   // thread-safe init
    return codes(t.lc, t.dc);
  }
  bool dynamic() {
    static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int nlen = (int)bits(5) + 257, ndist = (int)bits(5) + 1, ncode = (int)bits(4) + 4;
    if (failed) return false;
    if (nlen > 286 || ndist > 30) { fail(Err::DecodeFailure, "bad dynamic counts"); return false; }
    uint16_t lengths[320]; std::memset(lengths, 0, sizeof(lengths));
    int idx = 0;
    for (; idx < ncode; ++idx) lengths[order[idx]] = (uint16_t)bits(3);
    for (; idx < 19; ++idx) lengths[order[idx]] = 0;
    if (failed) return false;
    Huff lc, dc;
    if (build(lc, lengths, 19) != 0) { fail(Err::DecodeFailure, "incomplete code-length code"); return false; }
    idx = 0;
    while (idx < nlen + ndist) {
      int sym = decode(lc);
      if (failed) return false;
      if (sym < 16) lengths[idx++] = (uint16_t)sym;
      else {
        int len = 0, rep;
        if (sym == 16) { if (idx == 0) { fail(Err::DecodeFailure, "repeat with no previous length"); return false; } len = lengths[idx - 1]; rep = 3 + (int)bits(2); }
        else if (sym == 17) rep = 3 + (int)bits(3);
        else rep = 11 + (int)bits(7);
        if (failed) return false;
        if (idx + rep > nlen + ndist) { fail(Err::DecodeFailure, "too many lengths"); return false; }
        while (rep--) lengths[idx++] = (uint16_t)len;
      }
    }
    if (lengths[256] == 0) { fail(Err::DecodeFailure, "no end-of-block code"); return false; }
    int err = build(lc, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) { fail(Err::DecodeFailure, "bad literal/length code"); return false; }
    err = build(dc, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) { fail(Err::DecodeFailure, "bad distance code"); return false; }
    return codes(lc, dc);
  }
};
} // namespace

Status inflateStream(InflateIO& io, uint64_t maxOut, uint64_t& produced) {
  std::unique_ptr<Dec> d(new Dec(io, maxOut));
  int last;
  do {
    last = (int)d->bits(1);
    int type = (int)d->bits(2);
    if (d->failed) break;
    bool ok = type == 0 ? d->stored() : type == 1 ? d->fixed() : type == 2 ? d->dynamic() : (d->fail(Err::DecodeFailure, "invalid block type"), false);
    if (!ok || d->failed) break;
  } while (!last);
  produced = d->produced;
  if (d->failed) return Status::Fail(d->ferr, d->fmsg, false);
  if (d->wpos && !d->flushWin(d->wpos)) return Status::Fail(d->ferr, d->fmsg, false);
  return Status::Ok();
}

} // namespace persist
} // namespace ss
