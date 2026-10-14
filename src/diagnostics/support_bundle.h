// MOD-16 support-bundle exporter (spec §16). Flow: preview() -> user approves categories -> exportZip/exportFolder.
// Everything is re-sanitized on export (defense in depth) and the output is self-checked to be redaction-stable;
// if it is not, export FAILS rather than leaking. Only text logs/journal/system info/attached text sections are ever
// included: raw audio, camera frames, stream keys, show media, crash dumps have no code path into a bundle.
#pragma once
#include "core/types.h"
#include "core/security.h"
#include "diagnostics/event_journal.h"
#include "diagnostics/rotating_file.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ss {

struct BundleCategory {
  std::string id;            // "logs", "journal", "system", plus caller-defined text sections ("benchmark", ...)
  std::string title;         // shown in the preview list
  std::string description;   // what exactly is included
  size_t files = 0;
  uint64_t approxBytes = 0;
};
struct BundleLimits { size_t maxFileBytes = 6u << 20; size_t maxLineBytes = 4096; };
struct BundleReport { size_t files = 0; uint64_t bytes = 0; std::vector<std::string> names; };

class SupportBundle {
 public:
  SupportBundle(IFileOps& ops, IClock& clk, BundleLimits lim = BundleLimits());
  void setRedactor(const sec::Redactor* r) { red_ = r; }
  void addLogSource(RotatingFile* f) { logs_.push_back(f); }                   // category "logs"
  void setJournal(EventJournal* j, RotatingFile* jf) { journal_ = j; journalFile_ = jf; }  // category "journal"
  void setSystemInfo(const std::map<std::string, std::string>& kv) { sys_ = kv; }  // category "system"
  // Caller-defined sanitized text (e.g. benchmark summary, adaptation history). fileName must be a safe relative .txt/.json/.log name.
  Status addTextSection(const std::string& id, const std::string& title, const std::string& fileName, const std::string& content);
  // Step 1: list of categories (and what is always excluded). export*() refuses to run before this was called.
  std::vector<BundleCategory> preview();
  static std::vector<std::string> alwaysExcluded();
  // Step 2: approved ids must be a subset of preview() ids. The destination must not exist.
  Status exportZip(const std::string& destPath, const std::set<std::string>& approved, BundleReport* rep = nullptr);
  Status exportFolder(const std::string& destDir, const std::set<std::string>& approved, BundleReport* rep = nullptr);
  // True for names that must never be bundled (audio/video/images/dumps/key material).
  static bool isExcludedArtifactName(const std::string& name);
 private:
  struct Ent { std::string category, name; std::function<bool(std::string&)> load; };
  struct Sec { std::string id, title, file, content; };
  std::vector<Ent> plan(const std::set<std::string>& approved);
  bool sanitizeText(const std::string& in, std::string& out) const;
  bool sanitizeJournal(const std::string& in, std::string& out) const;
  Status buildEntries(const std::set<std::string>& approved, std::function<Status(const std::string&, const std::string&)> emit, BundleReport* rep);
  IFileOps& ops_; IClock& clk_; BundleLimits lim_; const sec::Redactor* red_ = nullptr;
  std::vector<RotatingFile*> logs_; EventJournal* journal_ = nullptr; RotatingFile* journalFile_ = nullptr;
  std::map<std::string, std::string> sys_; std::vector<Sec> secs_; bool previewed_ = false; std::set<std::string> previewIds_;
};

} // namespace ss
