// Allowlisted video decoder registry (spec §13/§16). Availability is reported from the PACKAGE MANIFEST
// ("decoders" section of assets/manifest.json) intersected with what is actually compiled in - never inferred from a
// filename. Fail-closed: a decoder that is not both registered and enabled in the manifest is unavailable.
#pragma once
#include "core/ports.h"
#include "media/import.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ss { namespace media {

struct DecoderEntry {
  std::string id;                                   // e.g. "mf-h264"
  std::vector<std::string> containers;              // "mp4" "mov" "mkv"
  std::vector<std::string> codecs;                  // "h264" "h265" "vp8" "vp9" "av1"
  std::function<std::unique_ptr<IVideoDecoder>()> factory;
};
struct DecoderAvailability {
  std::string id; bool compiled = false, enabledByManifest = false, available = false;
  std::vector<std::string> containers, codecs; std::string reason;
};

class DecoderRegistry {
 public:
  void registerDecoder(DecoderEntry e);             // compile-time allowlist (adapters call this at startup)
  // Parses {"decoders":[{"id":"mf-h264","enabled":true}, ...]} (the manifest top-level object or just that section).
  Status loadManifestAvailability(const std::string& manifestJson);
  bool isAvailable(const std::string& container, const std::string& codec) const;
  std::unique_ptr<IVideoDecoder> create(const std::string& container, const std::string& codec) const;
  std::vector<DecoderAvailability> report() const;
  size_t registeredCount() const { return entries_.size(); }
 private:
  const DecoderEntry* find(const std::string& container, const std::string& codec) const;
  std::vector<DecoderEntry> entries_;
  std::vector<std::string> enabledIds_;
  bool manifestLoaded_ = false;
};

// Container key from extension ("mp4"/"mov"/"mkv", else "").
std::string videoContainerKey(const std::string& path);
// Bounded scan of the file head for the codec sample-entry / codec-id string. "" when it cannot be determined.
std::string probeVideoCodec(const uint8_t* head, size_t n, Container c);

}} // namespace ss::media
