// ShowDocument <-> JSON (IF-11). Strict decode: limits checked before allocation, full validateShow() before return.
// File envelope: {"format":"stagesignal.show","checksum":"sha256:<hex of compact dump of 'show'>","show":{...}}
#pragma once
#include "core/show/model.h"
#include <string>
#include <vector>

namespace ss {
namespace persist {

constexpr const char* kShowFormat = "stagesignal.show";

struct DecodeInfo {
  int fromMajor = 0, fromMinor = 0;
  bool migrated = false;
  std::vector<std::string> warnings;     // unsupported/ignored fields from newer minors, etc.
};

std::string encodeShow(const show::ShowDocument& doc);                                      // envelope text (compact)
Status decodeShow(const uint8_t* d, size_t n, show::ShowDocument& out, DecodeInfo* info);   // out untouched unless Ok
// Raw show-object conversions (no envelope/checksum) for tests and tools.
std::string showObjectJson(const show::ShowDocument& doc);
// Peek the schema version without full parsing (envelope-aware). false if unreadable.
bool peekSchema(const uint8_t* d, size_t n, int& major, int& minor);

// Strip physical device ids and secret-like keys from opaque JSON and drop the journal (portable export, REQ-B03/S13).
// Returns the paths removed (for the export preview).
std::vector<std::string> sanitizeForExport(show::ShowDocument& doc);

} // namespace persist
} // namespace ss
