#include "persistence/zip.h"
#include "core/show/model.h"
#include "persistence/hash.h"
#include "persistence/inflate.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace ss {
namespace persist {
namespace {
void p16(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void p32(std::vector<uint8_t>& v, uint32_t x) { p16(v, x & 0xFFFF); p16(v, x >> 16); }
uint32_t g16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
uint32_t g32(const uint8_t* p) { return g16(p) | (g16(p + 2) << 16); }
Status bad(const std::string& why) { return Status::Fail(Err::InvalidArgument, "archive rejected: " + why, false); }

bool reservedName(std::string seg) {
  size_t dot = seg.find('.');
  if (dot != std::string::npos) seg.resize(dot);
  for (auto& c : seg) c = (char)tolower((unsigned char)c);
  if (seg == "con" || seg == "prn" || seg == "aux" || seg == "nul" || seg == "conin$" || seg == "conout$") return true;
  if (seg.size() == 4 && (seg.compare(0, 3, "com") == 0 || seg.compare(0, 3, "lpt") == 0) && seg[3] >= '0' && seg[3] <= '9') return true;
  return false;
}
} // namespace

Status checkMemberName(const std::string& raw, std::string& canon, bool* isDirOut) {
  canon.clear();
  if (raw.empty()) return bad("empty member name");
  if (raw.size() > 255) return bad("member name too long");
  for (unsigned char c : raw) {
    if (c < 0x20 || c == 0x7f) return bad("control character in member name");
    if (c >= 0x80) return bad("non-ASCII member name");
    if (c == '\\') return bad("backslash in member name");
    if (c == ':') return bad("drive/stream marker in member name");
    if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') return bad("reserved character in member name");
  }
  if (raw[0] == '/') return bad("absolute or UNC member path");
  std::string n = raw;
  bool dir = false;
  if (n.back() == '/') { dir = true; n.pop_back(); if (n.empty()) return bad("empty member name"); }
  size_t start = 0;
  for (size_t i = 0; i <= n.size(); ++i) {
    if (i == n.size() || n[i] == '/') {
      std::string seg = n.substr(start, i - start);
      if (seg.empty()) return bad("empty path segment");
      if (seg == "." || seg == "..") return bad("path traversal segment");
      if (seg.back() == '.' || seg.back() == ' ') return bad("trailing dot/space in segment");
      if (reservedName(seg)) return bad("reserved device name");
      start = i + 1;
    }
  }
  canon = n;
  for (auto& c : canon) c = (char)tolower((unsigned char)c);
  if (isDirOut) *isDirOut = dir;
  return Status::Ok();
}

// ---- writer ---------------------------------------------------------------------------------
Status ZipWriter::put(const void* d, size_t n) { Status s = out_.write(d, n); if (s.ok()) off_ += n; return s; }

Status ZipWriter::header(const std::string& name, uint32_t crc, uint32_t size) {
  std::string canon; Status s = checkMemberName(name, canon);
  if (!s.ok()) return s;
  if (off_ > 0xF0000000ull) return Status::Fail(Err::ResourceExhausted, "archive too large for non-zip64 writer");
  std::vector<uint8_t> h;
  p32(h, 0x04034b50); p16(h, 10); p16(h, 0); p16(h, 0); p16(h, 0); p16(h, 0x0021);   // fixed DOS time/date: reproducible
  p32(h, crc); p32(h, size); p32(h, size); p16(h, (uint32_t)name.size()); p16(h, 0);
  h.insert(h.end(), name.begin(), name.end());
  E e; e.name = name; e.crc = crc; e.size = size; e.off = (uint32_t)off_;
  ents_.push_back(e);
  return put(h.data(), h.size());
}

Status ZipWriter::addBytes(const std::string& name, const void* d, size_t n) {
  if (n > 0xF0000000ull) return Status::Fail(Err::ResourceExhausted, "member too large");
  Status s = header(name, crc32(d, n), (uint32_t)n);
  return s.ok() ? put(d, n) : s;
}

Status ZipWriter::addFile(IFileOps& ops, const std::string& name, const std::string& src, std::string* shaOut, uint64_t* sizeOut) {
  std::unique_ptr<IReadFile> r;
  Status s = ops.openRead(src, r);
  if (!s.ok()) return s;
  uint64_t sz = r->size();
  if (sz > 0xF0000000ull) return Status::Fail(Err::ResourceExhausted, "member too large for non-zip64 writer");
  std::vector<uint8_t> buf(64 * 1024);
  uint32_t crc = 0; Sha256 sha; uint64_t seen = 0;
  for (;;) {
    size_t got = 0; s = r->read(buf.data(), buf.size(), got);
    if (!s.ok()) return s;
    if (!got) break;
    crc = crc32Update(crc, buf.data(), got); sha.update(buf.data(), got); seen += got;
    if (got < buf.size()) break;
  }
  if (seen != sz) return Status::Fail(Err::InternalFault, "source changed while reading");
  s = header(name, crc, (uint32_t)sz);
  if (!s.ok()) return s;
  s = r->seek(0);
  if (!s.ok()) return s;
  uint64_t copied = 0;
  for (;;) {
    size_t got = 0; s = r->read(buf.data(), buf.size(), got);
    if (!s.ok()) return s;
    if (!got) break;
    copied += got;
    if (copied > sz) return Status::Fail(Err::InternalFault, "source grew while reading");
    s = put(buf.data(), got);
    if (!s.ok()) return s;
    if (got < buf.size()) break;
  }
  if (copied != sz) return Status::Fail(Err::InternalFault, "source changed while reading");
  if (shaOut) *shaOut = sha.finalHex();
  if (sizeOut) *sizeOut = sz;
  return Status::Ok();
}

Status ZipWriter::finish() {
  if (ents_.size() > 0xFFFF) return Status::Fail(Err::ResourceExhausted, "too many entries");
  std::vector<uint8_t> cd;
  uint64_t cdStart = off_;
  for (const E& e : ents_) {
    p32(cd, 0x02014b50); p16(cd, 20); p16(cd, 10); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0x0021);
    p32(cd, e.crc); p32(cd, e.size); p32(cd, e.size); p16(cd, (uint32_t)e.name.size());
    p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, 0); p32(cd, e.off);
    cd.insert(cd.end(), e.name.begin(), e.name.end());
  }
  uint32_t cdSize = (uint32_t)cd.size();
  p32(cd, 0x06054b50); p16(cd, 0); p16(cd, 0); p16(cd, (uint32_t)ents_.size()); p16(cd, (uint32_t)ents_.size());
  p32(cd, cdSize); p32(cd, (uint32_t)cdStart); p16(cd, 0);
  return put(cd.data(), cd.size());
}

// ---- reader ---------------------------------------------------------------------------------
int ZipReader::find(const std::string& canon) const {
  for (size_t i = 0; i < ents_.size(); ++i) if (ents_[i].canon == canon) return (int)i;
  return -1;
}

Status ZipReader::open(IFileOps& ops, const std::string& path, const NamePolicy& allow) {
  ents_.clear(); total_ = 0; f_.reset();
  Status s = ops.openRead(path, f_);
  if (!s.ok()) return s;
  const uint64_t fsz = f_->size();
  if (fsz < 22) return bad("file too small");
  // End-of-central-directory search within the last 64 KiB + 22 bytes.
  size_t tailLen = (size_t)std::min<uint64_t>(fsz, 22 + 65535);
  std::vector<uint8_t> tail(tailLen);
  s = f_->seek(fsz - tailLen); if (!s.ok()) return s;
  size_t got = 0; s = f_->read(tail.data(), tailLen, got); if (!s.ok()) return s;
  if (got != tailLen) return bad("short read");
  int eocd = -1;
  for (int i = (int)tailLen - 22; i >= 0; --i)
    if (g32(&tail[(size_t)i]) == 0x06054b50) { eocd = i; break; }
  if (eocd < 0) return bad("no end-of-central-directory record");
  const uint8_t* e = &tail[(size_t)eocd];
  uint64_t eocdPos = fsz - tailLen + (uint64_t)eocd;
  uint32_t disk = g16(e + 4), cdDisk = g16(e + 6), nThis = g16(e + 8), nTotal = g16(e + 10), commentLen = g16(e + 20);
  uint32_t cdSize = g32(e + 12), cdOff = g32(e + 16);
  if (nThis == 0xFFFF || nTotal == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) return bad("zip64 not supported");
  if (disk != 0 || cdDisk != 0 || nThis != nTotal) return bad("multi-disk archives not supported");
  if (eocdPos + 22 + commentLen != fsz) return bad("trailing data after end record");
  if (nTotal > show::limits::kMaxArchiveEntries) return bad("too many entries (max 10000)");       // before allocation
  if ((uint64_t)cdOff + cdSize > eocdPos) return bad("central directory out of range");
  if (cdSize > (uint64_t)nTotal * (46 + 255 + 4096 + 1024)) return bad("central directory size inconsistent");
  if (cdSize < (uint64_t)nTotal * 46) return bad("central directory too small");
  std::vector<uint8_t> cd(cdSize);
  s = f_->seek(cdOff); if (!s.ok()) return s;
  s = f_->read(cd.data(), cdSize, got); if (!s.ok()) return s;
  if (got != cdSize) return bad("short read of central directory");

  std::set<std::string> seen;
  size_t p = 0;
  ents_.reserve(nTotal);
  for (uint32_t i = 0; i < nTotal; ++i) {
    if (p + 46 > cd.size() || g32(&cd[p]) != 0x02014b50) return bad("bad central directory entry");
    const uint8_t* c = &cd[p];
    uint32_t madeBy = g16(c + 4), flags = g16(c + 8), method = g16(c + 10), crc = g32(c + 16);
    uint32_t csize = g32(c + 20), usize = g32(c + 24), nameLen = g16(c + 28), extraLen = g16(c + 30), cmtLen = g16(c + 32);
    uint32_t diskStart = g16(c + 34), extAttr = g32(c + 38), lho = g32(c + 42);
    if (nameLen > 255 || extraLen > 4096 || cmtLen > 1024) return bad("oversized entry fields");
    if (p + 46 + nameLen + extraLen + cmtLen > cd.size()) return bad("entry exceeds central directory");
    if (flags & 0x0001 || flags & 0x0040) return bad("encrypted entries not supported");
    if (method != 0 && method != 8) return bad("unsupported compression method");
    if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) return bad("zip64 entries not supported");
    if (diskStart != 0) return bad("multi-disk entry");
    if ((madeBy >> 8) == 3) { uint32_t mode = extAttr >> 16; if ((mode & 0170000) == 0120000) return bad("symbolic link entry"); }
    if (extAttr & 0x400) return bad("reparse-point entry");
    std::string raw((const char*)c + 46, nameLen), canon; bool isDir = false;
    s = checkMemberName(raw, canon, &isDir);
    if (!s.ok()) return s;
    if (!seen.insert(canon).second) return bad("duplicate canonical member path");
    if (allow && !allow(canon, isDir)) return bad("undeclared member '" + canon + "'");
    if (method == 0 && csize != usize) return bad("stored size mismatch");
    if (isDir && (usize || csize)) return bad("directory entry with data");
    total_ += usize;
    if (total_ > show::limits::kMaxDecompressed) return bad("decompressed size exceeds 2 GiB");
    if (method == 8 && usize > (1u << 20) && (uint64_t)usize > (uint64_t)csize * 200) return bad("excessive compression ratio (archive bomb)");
    if (method == 8 && usize > 0 && csize == 0) return bad("empty compressed data");
    if (lho >= cdOff) return bad("local header offset out of range");
    ZipEntry z; z.name = raw; z.canon = canon; z.isDir = isDir; z.method = (uint16_t)method; z.crc = crc;
    z.csize = csize; z.usize = usize; z.localOffset = lho;
    ents_.push_back(z);
    p += 46 + nameLen + extraLen + cmtLen;
  }
  if (p != cd.size()) return bad("trailing bytes in central directory");

  // Local headers: names must match, data ranges inside the file before the central directory and must not overlap.
  struct R { uint64_t a, b; };
  std::vector<R> ranges;
  for (ZipEntry& z : ents_) {
    uint8_t lh[30];
    s = f_->seek(z.localOffset); if (!s.ok()) return s;
    s = f_->read(lh, 30, got); if (!s.ok()) return s;
    if (got != 30 || g32(lh) != 0x04034b50) return bad("bad local header");
    uint32_t ln = g16(lh + 26), le = g16(lh + 28);
    if (ln != z.name.size()) return bad("local/central name mismatch");
    std::string lname(ln, '\0');
    if (ln) { s = f_->read(&lname[0], ln, got); if (!s.ok()) return s; if (got != ln) return bad("short local header"); }
    if (lname != z.name) return bad("local/central name mismatch");
    z.dataOffset = z.localOffset + 30 + ln + le;
    if (z.dataOffset + z.csize > cdOff) return bad("member data overlaps central directory");
    ranges.push_back({z.localOffset, z.dataOffset + z.csize});
  }
  std::sort(ranges.begin(), ranges.end(), [](const R& x, const R& y) { return x.a < y.a; });
  for (size_t i = 1; i < ranges.size(); ++i) if (ranges[i].a < ranges[i - 1].b) return bad("overlapping members");
  return Status::Ok();
}

Status ZipReader::extract(size_t idx, const std::function<bool(const uint8_t*, size_t)>& sink) {
  if (idx >= ents_.size()) return Status::Fail(Err::InvalidArgument, "bad entry index");
  const ZipEntry& z = ents_[idx];
  Status s = f_->seek(z.dataOffset);
  if (!s.ok()) return s;
  uint32_t crc = 0; uint64_t produced = 0;
  auto out = [&](const uint8_t* d, size_t n) { crc = crc32Update(crc, d, n); return sink(d, n); };
  if (z.method == 0) {
    std::vector<uint8_t> buf(64 * 1024);
    uint64_t left = z.csize;
    while (left) {
      size_t want = (size_t)std::min<uint64_t>(left, buf.size()), got = 0;
      s = f_->read(buf.data(), want, got);
      if (!s.ok()) return s;
      if (got != want) return bad("truncated member data");
      if (!out(buf.data(), got)) return Status::Fail(Err::Cancelled, "extraction sink refused data");
      left -= got; produced += got;
    }
  } else {
    uint64_t left = z.csize; bool readFail = false;
    InflateIO io;
    io.in = [&](uint8_t* b, size_t n) -> size_t {
      size_t want = (size_t)std::min<uint64_t>(left, n), got = 0;
      if (!want) return 0;
      if (!f_->read(b, want, got).ok()) { readFail = true; return 0; }
      left -= got; return got;
    };
    io.out = out;
    s = inflateStream(io, z.usize, produced);   // never emits more than the declared size
    if (!s.ok()) return readFail ? Status::Fail(Err::InternalFault, "read failure") : s;
  }
  if (produced != z.usize) return bad("decompressed size differs from declared size");
  if (crc != z.crc) return bad("CRC mismatch");
  return Status::Ok();
}

Status ZipReader::extractToMemory(size_t idx, size_t maxBytes, std::vector<uint8_t>& out) {
  if (idx >= ents_.size()) return Status::Fail(Err::InvalidArgument, "bad entry index");
  if (ents_[idx].usize > maxBytes) return Status::Fail(Err::ResourceExhausted, "member exceeds size limit");   // before allocation
  out.clear(); out.reserve((size_t)ents_[idx].usize);
  return extract(idx, [&](const uint8_t* d, size_t n) { out.insert(out.end(), d, d + n); return true; });
}

} // namespace persist
} // namespace ss
