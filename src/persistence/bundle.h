// Portable .stagesignal bundle (spec §17): ZIP with show.json, manifest.json, media/<sha256>.<ext>.
// Export strips physical device ids and secrets; import is hostile-input safe and publishes atomically.
#pragma once
#include "core/show/model.h"
#include "persistence/file_ops.h"
#include <string>
#include <vector>

namespace ss {
namespace persist {

struct ExportOptions {
  bool includeMedia = true;
  bool allowMissingMedia = false;    // true: export without absent files and report them; false: fail listing them
  std::string mediaRoot;             // directory that relPath entries resolve against
};
struct ExportReport {
  std::vector<std::string> removedFields;    // physical ids / secrets stripped (export preview)
  std::vector<std::string> missingMedia;     // hashes not found on disk
  size_t mediaIncluded = 0;
  uint64_t archiveBytes = 0;
};
// Writes <destZip> via temp file + flush + atomic rename. Media bytes are re-hashed against their declared hash.
Status exportBundle(IFileOps& ops, const show::ShowDocument& doc, const ExportOptions& opt, const std::string& destZip, ExportReport* report = nullptr);

struct ImportResult {
  Status status;
  show::ShowDocument doc;                    // valid only if status.ok()
  std::string showDir;                       // published directory (contains show.json, manifest.json, media/)
  std::string showFile;                      // showDir/show.json
  std::string mediaRoot;                     // == showDir; doc.media[].relPath resolve against it
  std::vector<std::string> warnings;         // e.g. media not included in bundle
  bool mediaComplete = false;
};
// Validates the whole archive (structure, names, limits, manifest, hashes, full document) while extracting into a
// private staging directory next to destDir, then atomically renames it to destDir (which must not exist).
// On any failure nothing is published and the staging directory is removed.
ImportResult importBundle(IFileOps& ops, const std::string& archivePath, const std::string& destDir);

} // namespace persist
} // namespace ss
