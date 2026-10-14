#include "persistence/bundle.h"
#include "nlohmann/json.hpp"
#include "persistence/hash.h"
#include "persistence/show_json.h"
#include "persistence/zip.h"
#include <algorithm>
#include <map>
#include <set>

namespace ss {
namespace persist {
using json = nlohmann::json;
using namespace show;

namespace {
constexpr const char* kManifestFormat = "stagesignal.bundle";

std::string extOf(const std::string& fmt) { return fmt == "jpeg" ? "jpg" : fmt; }

Status abortWith(IFileOps& ops, const std::string& tmp, Status s) { ops.remove(tmp); return s; }
}

Status exportBundle(IFileOps& ops, const ShowDocument& docIn, const ExportOptions& opt, const std::string& destZip, ExportReport* report) {
  ExportReport local; ExportReport& rep = report ? *report : local;
  rep = ExportReport();
  Status v = validateShow(docIn);
  if (!v.ok()) return v;
  ShowDocument doc = docIn;
  rep.removedFields = sanitizeForExport(doc);

  // Resolve media: canonical in-archive names; hashes re-verified against source bytes.
  struct M { size_t idx; std::string src, member; bool include; };
  std::vector<M> ms;
  uint64_t budget = 0;
  for (size_t i = 0; i < doc.media.size(); ++i) {
    MediaRecord& m = doc.media[i];
    M x; x.idx = i; x.member = "media/" + m.hash + "." + extOf(m.format); x.include = false;
    if (opt.includeMedia) {
      x.src = joinPath(opt.mediaRoot, m.relPath);
      if (ops.exists(x.src) && !ops.isSymlink(x.src)) x.include = true;
      else { rep.missingMedia.push_back(m.hash); }
    }
    ms.push_back(x);
  }
  if (!rep.missingMedia.empty() && !opt.allowMissingMedia) {
    std::string list; for (const auto& h : rep.missingMedia) list += (list.empty() ? "" : ", ") + h.substr(0, 12);
    return Status::Fail(Err::InvalidArgument, "media file(s) missing, cannot export: " + list);
  }
  for (auto& x : ms) doc.media[x.idx].relPath = x.member;     // portable canonical location
  (void)budget;

  std::string tmp = destZip + ".tmp";
  std::unique_ptr<IWriteFile> w;
  Status s = ops.openWrite(tmp, w);
  if (!s.ok()) return abortWith(ops, tmp, s);
  ZipWriter zw(*w);
  std::string showText = encodeShow(doc);
  s = zw.addBytes("show.json", showText.data(), showText.size());
  if (!s.ok()) return abortWith(ops, tmp, s);

  json files = json::array();
  files.push_back({{"path", "show.json"}, {"size", showText.size()}, {"sha256", Sha256::hex(showText.data(), showText.size())}});
  json mediaList = json::array();
  uint64_t total = showText.size();
  for (auto& x : ms) {
    const MediaRecord& m = doc.media[x.idx];
    if (!x.include) { mediaList.push_back({{"hash", m.hash}, {"path", x.member}, {"included", false}}); continue; }
    std::string sha; uint64_t sz = 0;
    s = zw.addFile(ops, x.member, x.src, &sha, &sz);
    if (!s.ok()) return abortWith(ops, tmp, s);
    if (sha != m.hash) return abortWith(ops, tmp, Status::Fail(Err::InvalidArgument, "media content does not match its declared hash: " + m.hash.substr(0, 12)));
    total += sz;
    if (total > limits::kMaxDecompressed) return abortWith(ops, tmp, Status::Fail(Err::ResourceExhausted, "export exceeds 2 GiB"));
    files.push_back({{"path", x.member}, {"size", sz}, {"sha256", sha}});
    mediaList.push_back({{"hash", m.hash}, {"path", x.member}, {"included", true}});
    ++rep.mediaIncluded;
  }
  json manifest = {{"format", kManifestFormat}, {"version", 1}, {"showUuid", doc.uuid}, {"schema", {{"major", doc.schemaMajor}, {"minor", doc.schemaMinor}}},
                   {"mediaIncluded", opt.includeMedia && rep.missingMedia.empty()}, {"files", files}, {"media", mediaList},
                   {"excluded", {"secrets", "physical-device-ids", "journal"}}, {"removedFieldCount", rep.removedFields.size()}};
  std::string mt = manifest.dump();
  s = zw.addBytes("manifest.json", mt.data(), mt.size());
  if (!s.ok()) return abortWith(ops, tmp, s);
  s = zw.finish();
  if (!s.ok()) return abortWith(ops, tmp, s);
  rep.archiveBytes = zw.bytesWritten();
  s = w->flushAndClose();
  if (!s.ok()) return abortWith(ops, tmp, s);
  w.reset();
  s = ops.rename(tmp, destZip);
  if (!s.ok()) return abortWith(ops, tmp, s);
  return Status::Ok();
}

// ---- import --------------------------------------------------------------------------------------------
namespace {
bool isMediaMember(const std::string& canon, std::string* hashOut = nullptr) {
  // media/<64 hex>.<ext> with ext in allowlist (canon is lower-case)
  if (canon.compare(0, 6, "media/") != 0) return false;
  std::string f = canon.substr(6);
  size_t dot = f.find('.');
  if (dot != 64 || f.find('/') != std::string::npos) return false;
  std::string h = f.substr(0, 64), ext = f.substr(65);
  if (!isHex(h, 64, 64)) return false;
  size_t n = 0; const char* const* fm = mediaFormats(&n); bool ok = false;
  for (size_t i = 0; i < n; ++i) if (ext == extOf(fm[i])) ok = true;
  if (!ok) return false;
  if (hashOut) *hashOut = h;
  return true;
}
}

ImportResult importBundle(IFileOps& ops, const std::string& archivePath, const std::string& destDir) {
  ImportResult res;
  auto fail = [&](Status s) { res.status = s; return res; };
  if (ops.exists(destDir)) return fail(Status::Fail(Err::InvalidArgument, "import destination already exists"));
  ZipReader zr;
  Status s = zr.open(ops, archivePath, [](const std::string& canon, bool isDir) {
    if (isDir) return canon == "media";
    return canon == "show.json" || canon == "manifest.json" || isMediaMember(canon);
  });
  if (!s.ok()) return fail(s);
  int ish = zr.find("show.json"), imf = zr.find("manifest.json");
  if (ish < 0 || imf < 0) return fail(Status::Fail(Err::InvalidArgument, "archive rejected: show.json and manifest.json are required", false));
  // Archive members must use exact canonical names (case-folding aliases like SHOW.JSON are rejected).
  for (const auto& e : zr.entries()) if (!e.isDir && e.name != e.canon) return fail(Status::Fail(Err::InvalidArgument, "archive rejected: member names must be lower-case", false));

  std::vector<uint8_t> showBytes, mfBytes;
  s = zr.extractToMemory((size_t)ish, limits::kMaxJsonBytes, showBytes);
  if (!s.ok()) return fail(s);
  s = zr.extractToMemory((size_t)imf, limits::kMaxJsonBytes, mfBytes);
  if (!s.ok()) return fail(s);

  ShowDocument doc; DecodeInfo di;
  s = decodeShow(showBytes.data(), showBytes.size(), doc, &di);          // full validation incl. limits & schema
  if (!s.ok()) return fail(s);
  for (const auto& w : di.warnings) res.warnings.push_back(w);

  json mf = json::parse(mfBytes.begin(), mfBytes.end(), nullptr, false);
  if (mf.is_discarded() || !mf.is_object() || mf.value("format", "") != kManifestFormat || !mf.contains("files") || !mf["files"].is_array() || mf["files"].size() > limits::kMaxArchiveEntries)
    return fail(Status::Fail(Err::InvalidArgument, "manifest invalid", false));
  std::map<std::string, std::pair<uint64_t, std::string>> declared;
  for (const auto& f : mf["files"]) {
    if (!f.is_object() || !f.contains("path") || !f["path"].is_string() || !f.contains("size") || !f["size"].is_number_unsigned() || !f.contains("sha256") || !f["sha256"].is_string())
      return fail(Status::Fail(Err::InvalidArgument, "manifest file entry invalid", false));
    std::string canon; if (!checkMemberName(f["path"].get<std::string>(), canon).ok()) return fail(Status::Fail(Err::InvalidArgument, "manifest path invalid", false));
    declared[canon] = {f["size"].get<uint64_t>(), f["sha256"].get<std::string>()};
  }
  // show.json integrity against manifest.
  auto sj = declared.find("show.json");
  if (sj == declared.end() || sj->second.first != showBytes.size() || sj->second.second != Sha256::hex(showBytes.data(), showBytes.size()))
    return fail(Status::Fail(Err::InvalidArgument, "show.json does not match manifest", false));
  // Every media member must be declared by the show and by the manifest; sizes must agree.
  std::set<std::string> docHashes; for (const auto& m : doc.media) docHashes.insert(m.hash);
  std::set<std::string> present;
  for (size_t i = 0; i < zr.entries().size(); ++i) {
    const ZipEntry& e = zr.entries()[i]; std::string h;
    if (e.isDir || !isMediaMember(e.canon, &h)) continue;
    if (!docHashes.count(h)) return fail(Status::Fail(Err::InvalidArgument, "archive rejected: media member not referenced by the show", false));
    auto d = declared.find(e.canon);
    if (d == declared.end() || d->second.first != e.usize || d->second.second != h) return fail(Status::Fail(Err::InvalidArgument, "media member does not match manifest", false));
    present.insert(h);
  }
  for (const auto& d : declared) if (d.first != "show.json" && zr.find(d.first) < 0) {
    bool includedFalse = false;
    if (mf.contains("media") && mf["media"].is_array()) for (const auto& m : mf["media"]) if (m.is_object() && m.value("path", "") == d.first && m.value("included", true) == false) includedFalse = true;
    if (!includedFalse) return fail(Status::Fail(Err::InvalidArgument, "manifest lists a file missing from the archive", false));
  }
  res.mediaComplete = present.size() == docHashes.size();
  for (const auto& m : doc.media) if (!present.count(m.hash)) res.warnings.push_back("media not included in bundle: " + m.hash.substr(0, 12) + " (" + m.format + ")");
  // The loaded document's media paths must be the canonical in-bundle ones (never trust arbitrary paths from the archive).
  for (auto& m : doc.media) m.relPath = "media/" + m.hash + "." + extOf(m.format);

  // ---- extract into a private staging directory beside the destination ----
  std::string parent = parentDir(destDir);
  s = ops.createDirs(parent);
  if (!s.ok()) return fail(s);
  std::string stage;
  for (int n = 0; n < 1000; ++n) {
    stage = joinPath(parent, ".ss_import_" + Sha256::hex(destDir.data(), destDir.size()).substr(0, 8) + "_" + std::to_string(n));
    if (ops.createPrivateDir(stage).ok()) break;
    stage.clear();
  }
  if (stage.empty()) return fail(Status::Fail(Err::InternalFault, "cannot create private staging directory"));
  auto cleanup = [&](Status st) { ops.removeAll(stage); return fail(st); };

  std::string finalShow = encodeShow(doc);       // re-encoded with canonical relPaths
  s = writeFileAll(ops, joinPath(stage, "show.json"), finalShow.data(), finalShow.size());
  if (!s.ok()) return cleanup(s);
  s = writeFileAll(ops, joinPath(stage, "manifest.json"), mfBytes.data(), mfBytes.size());
  if (!s.ok()) return cleanup(s);
  if (!present.empty()) { s = ops.createDirs(joinPath(stage, "media")); if (!s.ok()) return cleanup(s); }
  for (size_t i = 0; i < zr.entries().size(); ++i) {
    const ZipEntry& e = zr.entries()[i]; std::string h;
    if (e.isDir || !isMediaMember(e.canon, &h)) continue;
    std::string outPath = joinPath(stage, e.canon);          // canon is validated: media/<hex>.<ext>, no traversal possible
    std::unique_ptr<IWriteFile> wf;
    s = ops.openWrite(outPath, wf);
    if (!s.ok()) return cleanup(s);
    Sha256 sha; Status ws;
    s = zr.extract(i, [&](const uint8_t* d, size_t n) { sha.update(d, n); ws = wf->write(d, n); return ws.ok(); });
    if (!s.ok()) return cleanup(ws.ok() ? s : ws);
    s = wf->flushAndClose();
    if (!s.ok()) return cleanup(s);
    if (sha.finalHex() != h) return cleanup(Status::Fail(Err::InvalidArgument, "media content does not match its hash", false));
  }
  s = ops.rename(stage, destDir);                             // atomic publish
  if (!s.ok()) return cleanup(s);
  res.doc = std::move(doc); res.showDir = destDir; res.showFile = joinPath(destDir, "show.json"); res.mediaRoot = destDir;
  res.status = Status::Ok();
  return res;
}

} // namespace persist
} // namespace ss
