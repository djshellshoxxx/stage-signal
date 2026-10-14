#include "media/sha256.h"
#include "media/import.h"
#include <cstdio>
#include <cstring>

namespace ss { namespace media {

namespace {
const uint32_t K[64] = {
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
 0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
 0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
 0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}

void Sha256::reset() {
  static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  std::memcpy(h_, iv, sizeof(h_)); len_ = 0; fill_ = 0;
}
void Sha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
    uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  uint32_t a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],h=h_[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25), ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22), mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
  }
  h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d; h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=h;
}
void Sha256::update(const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data; len_ += n;
  while (n) {
    size_t take = 64 - fill_; if (take > n) take = n;
    std::memcpy(buf_ + fill_, p, take); fill_ += take; p += take; n -= take;
    if (fill_ == 64) { block(buf_); fill_ = 0; }
  }
}
void Sha256::final(uint8_t out[32]) {
  uint64_t bits = len_ * 8;
  uint8_t pad = 0x80; update(&pad, 1);
  uint8_t z = 0; while (fill_ != 56) update(&z, 1);
  uint8_t l[8]; for (int i = 0; i < 8; ++i) l[i] = (uint8_t)(bits >> (56 - 8 * i));
  update(l, 8);
  for (int i = 0; i < 8; ++i) { out[i*4] = (uint8_t)(h_[i] >> 24); out[i*4+1] = (uint8_t)(h_[i] >> 16); out[i*4+2] = (uint8_t)(h_[i] >> 8); out[i*4+3] = (uint8_t)h_[i]; }
}
std::string Sha256::finalHex() {
  uint8_t d[32]; final(d);
  static const char* hx = "0123456789abcdef"; std::string s;
  for (int i = 0; i < 32; ++i) { s.push_back(hx[d[i] >> 4]); s.push_back(hx[d[i] & 15]); }
  return s;
}
std::string sha256Hex(const void* data, size_t n) { Sha256 s; s.update(data, n); return s.finalHex(); }

bool isHex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

Status sha256FileHex(const std::string& path, std::string& hexOut, uint64_t* sizeOut) {
  hexOut.clear();
  FILE* f = std::fopen(path.c_str(), "rb");   // trusted tooling path (callers validate entry paths); ASCII/ANSI fopen
  if (!f) return Status::Fail(Err::InvalidArgument, "cannot open file: " + baseName(path));
  Sha256 sh; uint8_t buf[65536]; uint64_t total = 0; size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { sh.update(buf, n); total += n; }
  bool err = std::ferror(f) != 0; std::fclose(f);
  if (err) return Status::Fail(Err::DecodeFailure, "read error: " + baseName(path));
  hexOut = sh.finalHex(); if (sizeOut) *sizeOut = total;
  return Status::Ok();
}

}} // namespace ss::media
