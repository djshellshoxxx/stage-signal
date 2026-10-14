// Portable H.264 / AAC bitstream helpers shared by the MKV writer (recording), the RTMP transport (broadcast) and the
// Windows Media Foundation encoder adapters. No codec is implemented here: only container-level plumbing (Annex B <->
// AVCC, avcC, AudioSpecificConfig). EncodedPacket/MediaPacket H.264 payloads are ANNEX B (00 00 01 / 00 00 00 01 start codes).
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
namespace ss { namespace h264 {
using Bytes = std::vector<uint8_t>;
struct Nal { const uint8_t* p = nullptr; size_t n = 0; int type() const { return n ? (p[0] & 31) : -1; } };
enum NalType { kSlice = 1, kIdr = 5, kSei = 6, kSps = 7, kPps = 8, kAud = 9 };
bool looksAnnexB(const uint8_t* d, size_t n);                       // starts with 00 00 01 or 00 00 00 01
// Splits an Annex B buffer into NAL units (views into d; trailing zero bytes removed). Empty NALs are skipped.
std::vector<Nal> splitAnnexB(const uint8_t* d, size_t n);
bool hasIdr(const uint8_t* annexB, size_t n);                       // true when an IDR slice (type 5) is present
// Annex B -> 4-byte-length-prefixed (AVCC/FLV/MKV sample). SPS/PPS/AUD are dropped when stripParamSets (they live in avcC).
// If `sps`/`pps` are non-null the first SPS/PPS seen are copied there. Returns false when no NAL was found.
bool annexBToAvcc(const uint8_t* d, size_t n, Bytes& out, bool stripParamSets, Bytes* sps = nullptr, Bytes* pps = nullptr);
// AVCDecoderConfigurationRecord from one SPS + one PPS (NAL bytes WITHOUT start code). Empty on bad input.
Bytes buildAvcC(const Bytes& sps, const Bytes& pps);
// Reads SPS/PPS back out of an avcC (first of each). Validates lengths; false on malformed input.
bool parseAvcC(const uint8_t* d, size_t n, Bytes& sps, Bytes& pps, int* nalLengthSize = nullptr);
// Convenience: avcC from the first SPS/PPS found in an Annex B buffer (empty when either is missing).
Bytes avcCFromAnnexB(const uint8_t* d, size_t n);
}  // namespace h264

namespace aac {
using Bytes = std::vector<uint8_t>;
// 2-byte AudioSpecificConfig for AAC-LC (objectType 2): 5 bits type, 4 bits freq index, 4 bits channel config, 3 zero bits.
// Empty when the rate is not one of the 13 standard rates or channels is not 1..2 (explicit 24-bit rate escape is not used).
Bytes audioSpecificConfig(uint32_t rate, uint32_t channels, int objectType = 2);
bool parseAudioSpecificConfig(const uint8_t* d, size_t n, int& objectType, uint32_t& rate, uint32_t& channels);
// Strips a 7/9-byte ADTS header if present (MFT can be configured to emit ADTS); returns false if `d` has none.
bool stripAdts(const uint8_t* d, size_t n, const uint8_t*& payload, size_t& payloadLen);
}  // namespace aac
}  // namespace ss
