#include "diagnostics/zip_store.h"
#include "diagnostics/time_util.h"
#include "core/security.h"

namespace ss {

uint32_t crc32(const void* data, size_t n, uint32_t seed) {
  static uint32_t table[256]; static bool init = false;
  if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; } init = true; }
  uint32_t c = ~seed; const unsigned char* p = (const unsigned char*)data;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}
namespace {
void le16(std::string& s, uint32_t v) { s += (char)(v & 255); s += (char)((v >> 8) & 255); }
void le32(std::string& s, uint32_t v) { le16(s, v & 0xFFFF); le16(s, v >> 16); }
}
bool ZipStoreWriter::put(const std::string& s) {
  if (failed_) return false;
  if (!s.empty() && !sink_(s.data(), s.size())) { failed_ = true; return false; }
  off_ += s.size(); return true;
}
Status ZipStoreWriter::addFile(const std::string& name, const std::string& data, int64_t wall) {
  if (done_ || failed_) return Status::Fail(Err::InternalFault, "zip writer closed");
  Status v = sec::validateRelPath(name); if (!v.ok()) return v;
  if (ents_.size() >= 65535) return Status::Fail(Err::ResourceExhausted, "too many zip entries");
  if (data.size() > 0xFFFFFFF0ull || off_ > 0xFFFFFF00ull - data.size()) return Status::Fail(Err::ResourceExhausted, "zip too large for 32-bit format");
  CivilTime c = civilFromUnixMs(wall); if (c.y < 1980) c = {1980, 1, 1, 0, 0, 0, 0}; if (c.y > 2107) c.y = 2107;
  Ent e; e.name = name; e.crc = crc32(data.data(), data.size()); e.size = (uint32_t)data.size(); e.offset = (uint32_t)off_;
  e.dosTime = (uint16_t)((c.h << 11) | (c.mi << 5) | (c.s / 2)); e.dosDate = (uint16_t)(((c.y - 1980) << 9) | (c.mo << 5) | c.d);
  std::string h; le32(h, 0x04034b50); le16(h, 10); le16(h, 0x0800); le16(h, 0); le16(h, e.dosTime); le16(h, e.dosDate);
  le32(h, e.crc); le32(h, e.size); le32(h, e.size); le16(h, (uint32_t)name.size()); le16(h, 0); h += name;
  if (!put(h) || !put(data)) return Status::Fail(Err::DiskFull, "write failed");
  ents_.push_back(e); return Status::Ok();
}
Status ZipStoreWriter::finish() {
  if (done_) return Status::Ok();
  uint64_t cdStart = off_; std::string cd;
  for (auto& e : ents_) {
    le32(cd, 0x02014b50); le16(cd, 20); le16(cd, 10); le16(cd, 0x0800); le16(cd, 0); le16(cd, e.dosTime); le16(cd, e.dosDate);
    le32(cd, e.crc); le32(cd, e.size); le32(cd, e.size); le16(cd, (uint32_t)e.name.size()); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0);
    le32(cd, 0); le32(cd, e.offset); cd += e.name;
  }
  std::string end; le32(end, 0x06054b50); le16(end, 0); le16(end, 0); le16(end, (uint32_t)ents_.size()); le16(end, (uint32_t)ents_.size());
  le32(end, (uint32_t)cd.size()); le32(end, (uint32_t)cdStart); le16(end, 0);
  done_ = true;
  if (!put(cd) || !put(end)) return Status::Fail(Err::DiskFull, "write failed");
  return Status::Ok();
}

} // namespace ss
