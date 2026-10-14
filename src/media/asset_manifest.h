// Asset provenance/licence manifest (spec §13 "Asset manifest fields", REQ-S15). Packaging calls verifyManifest(dir)
// and FAILS on any issue: absent/incomplete entry, missing file, size/SHA-256/dimension mismatch, unlisted file.
// A checksum proves byte identity only - not ownership or licence validity (spec §13).
#pragma once
#include "core/types.h"
#include <string>
#include <vector>

namespace ss { namespace media {

struct FlashInfo { bool hasFlashing = false; bool hasStrobe = false; double maxFlashHz = 0; std::string notes; };

struct AssetEntry {
  std::string id, version, type /*still|animation|video|font|preset*/, path /*relative to manifest dir, '/' separators*/;
  std::string creator, originalUrl, retrievalDate /*YYYY-MM-DD*/;
  std::string license /*SPDX expression*/, copyright, attributionText, attributionLocation;
  int commercialUse = -1, modification = -1;                   // -1 = not stated (verification fails), 0 no, 1 yes
  std::string provenanceKind /*original|derivative|third-party*/, provenanceNotes;
  std::vector<std::string> derivedFrom;                         // asset ids / URLs for derivatives
  std::string sha256Original, sha256Distributed;
  int width = 0, height = 0; int64_t durationMs = 0; uint64_t sizeBytes = 0;
  FlashInfo flash;
  std::vector<std::string> tiers;                               // BASE STANDARD ENHANCED SHOW
};
struct DecoderDecl { std::string id; bool enabled = false; std::vector<std::string> containers, codecs; std::string notes; };
struct Manifest { int schemaVersion = 1; std::string generator; std::vector<AssetEntry> assets; std::vector<DecoderDecl> decoders; };

Status parseManifest(const std::string& json, Manifest& out);   // syntax/type errors only; completeness is verifyManifest's job
std::string serializeManifest(const Manifest& m);               // stable pretty JSON (used by tests/tools/gen_assets.cpp)
bool isValidSpdxExpression(const std::string& expr);            // syntax check (ids, AND/OR/WITH, parentheses, LicenseRef-)

struct ManifestIssue { std::string assetId; std::string code; std::string message; };
struct ManifestReport {
  bool ok = false;                       // true only when the manifest parsed and issues.empty()
  size_t entriesChecked = 0, filesScanned = 0;
  std::vector<ManifestIssue> issues;
  std::string summary() const;           // human-readable multi-line report
};
struct VerifyOptions {
  bool failOnUnlistedFiles = true;       // bundled file with no manifest entry fails packaging
  std::string requireTier;               // if set, only entries permitted for this tier are required to exist
};
// dir contains manifest.json and the asset files it references (paths relative to dir). Never throws.
ManifestReport verifyManifest(const std::string& dir, const VerifyOptions& opt = VerifyOptions());
ManifestReport verifyManifestText(const std::string& dir, const std::string& manifestJson, const VerifyOptions& opt = VerifyOptions());

}} // namespace ss::media
