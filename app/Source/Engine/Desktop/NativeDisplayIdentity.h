#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace ss::app {
struct NativeDisplayRecord {
    int x = 0, y = 0, width = 0, height = 0;
    std::string identity, connector;
};
// Message-thread snapshot. Missing or ambiguous geometry must never be guessed.
std::vector<NativeDisplayRecord> nativeDisplayIdentities();
const NativeDisplayRecord* uniqueNativeDisplay(const std::vector<NativeDisplayRecord>&, int x, int y, int width, int height);
// Returns an opaque macOS hardware identity, or empty when the tuple is incomplete.
std::string macDisplayIdentity(std::uint32_t vendor, std::uint32_t model, std::uint32_t serial);
}
