// Packaging gate (REQ-S15): fails (non-zero exit) when any bundled asset lacks a complete provenance/licence entry or
// differs from its recorded SHA-256 / size / dimensions, or when an unlisted file sits in the assets directory.
// NOT globbed by CMake. Build manually (or wire into packaging):
//   g++ -std=c++17 -O1 -Isrc -Ithird_party tools/asset_check.cpp src/media/asset_manifest.cpp src/media/import.cpp \
//       src/media/sha256.cpp src/media/stb_impl.cpp src/media/animation.cpp src/core/clock.cpp -pthread -o asset_check
//   ./asset_check assets [--tier BASE|STANDARD|ENHANCED|SHOW] [--allow-unlisted]
// In-process equivalent: ss::media::verifyManifest(dir, VerifyOptions) from media/asset_manifest.h.
#include "media/asset_manifest.h"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
  std::string dir = "assets"; ss::media::VerifyOptions opt;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--tier") && i + 1 < argc) opt.requireTier = argv[++i];
    else if (!std::strcmp(argv[i], "--allow-unlisted")) opt.failOnUnlistedFiles = false;
    else if (argv[i][0] == '-') { std::fprintf(stderr, "usage: asset_check <assets-dir> [--tier T] [--allow-unlisted]\n"); return 64; }
    else dir = argv[i];
  }
  ss::media::ManifestReport r = ss::media::verifyManifest(dir, opt);
  std::printf("%s", r.summary().c_str());
  return r.ok ? 0 : 1;
}
