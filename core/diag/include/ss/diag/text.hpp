// StageSignal diag: small locale-independent text helpers shared by the report, CSV export and log formatter.
#pragma once
#include <cstdint>
#include <string>
#include "ss/base/time.hpp"

namespace ss::diag {

// Escapes & < > " ' ` for HTML text and attribute contexts. Control characters (except \n, \t) are dropped and invalid
// UTF-8 bytes become '?', so the result is always well-formed UTF-8.
std::string htmlEscape(const std::string& s);

// RFC 4180 cell for TEXT: quotes when needed and neutralises spreadsheet formulas (leading = + - @ tab CR gets a
// leading apostrophe). Use csvNumber() for numeric cells.
std::string csvText(const std::string& s);
std::string csvNumber(const std::string& formatted);   // passes digits, '.', '-' only; anything else becomes empty

// Fixed-point formatting without printf/locale (round half away from zero). decimals 0..6.
std::string fixedDecimals(double v, int decimals);

// "2026-10-20T19:30:05Z" or with milliseconds "2026-10-20T19:30:05.123Z". Pure arithmetic, no time zone database.
std::string formatUtcIso(std::int64_t unixMs, bool withMillis = false);

std::string formatDuration(Ns ns);                    // "m:ss" or "h:mm:ss"; negative clamps to 0
std::string keyName(int tonic, bool minor);           // "A minor", "C# major"; "" when unknown

}  // namespace ss::diag
