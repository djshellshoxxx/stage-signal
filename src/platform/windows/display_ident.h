// Internal: pure display-identity helpers (EDID parsing, interface path splitting); exposed for the smoke test.
#pragma once
#include <cstdint>
#include <string>
namespace ss { namespace win {
struct EdidInfo { std::string mfg, name, serialText; uint16_t product = 0; uint32_t serial = 0; uint64_t hash = 0; };
bool parseEdid(const uint8_t* e, size_t n, EdidInfo& out);
bool splitInterfacePath(const std::string& path, std::string& model, std::string& instance);
}}
