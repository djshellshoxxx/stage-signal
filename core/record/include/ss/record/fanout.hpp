// StageSignal record: record-while-broadcasting coexistence (spec section 15: "Independent muxers/writers prevent
// slow network from stopping local recording ... bounded consumer queues and finite lifetime ... No disk/network
// write on render/audio threads").
//
// Design: the producer (render/audio side) calls SinkFanout::publish(), which only enqueues into per-consumer
// BOUNDED queues and never blocks, allocates a payload copy, or waits on a consumer. Each consumer (the record
// ffmpeg process feeder, the broadcast ffmpeg process feeder) drains its own queue on its own thread. A stuck
// network consumer can only overflow ITS queue; it cannot delay the disk consumer. Disk-queue overflow (the disk
// itself is too slow) is real data loss and is reported as loss ranges for the session summary, never hidden.
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "ss/base/time.hpp"
#include "ss/record/profile.hpp"

namespace ss::record {

struct MediaChunk {
    std::uint64_t seq = 0;
    Ns ptsNs = 0, durationNs = 0;
    bool keyframe = true;                                    // raw frames and PCM are all "key"
    std::shared_ptr<const std::vector<std::uint8_t>> data;  // shared between queues: published once, never copied
    std::size_t size() const { return data ? data->size() : 0; }
};

enum class OverflowPolicy : std::uint8_t {
    DropOldest,       // network: freshness over completeness
    DropNewest        // disk: keep what is already queued contiguous, count the rest as loss
};

struct LossRange { Ns fromNs = 0; Ns lengthNs = 0; };

struct QueueStats {
    std::uint64_t pushed = 0, popped = 0, dropped = 0, droppedBytes = 0;
    std::size_t depthBytes = 0, maxDepthBytes = 0;
};

class ConsumerQueue {
public:
    // requireKeyframeAfterDrop: after any drop, chunks are discarded until a keyframe so the consumer never starts
    // in the middle of a GOP (encoded packets). Raw frames/PCM set it false.
    ConsumerQueue(std::string name, std::size_t capacityBytes, OverflowPolicy policy, bool requireKeyframeAfterDrop = false);
    bool push(const MediaChunk& c);          // false if this chunk was not queued (dropped); queue itself never grows past capacity
    bool pop(MediaChunk& out);
    void clear();                            // e.g. on reconnect: abandon queued media
    bool takeKeyframeRequest();              // true once after drops: the producer should force a keyframe/resync
    std::vector<LossRange> takeLosses();     // merged contiguous dropped ranges since the last call
    const QueueStats& stats() const { return stats_; }
    const std::string& name() const { return name_; }
    std::size_t capacity() const { return cap_; }
    std::size_t size() const { return q_.size(); }
private:
    void noteLoss(const MediaChunk& c);
    std::string name_;
    std::size_t cap_;
    OverflowPolicy policy_;
    bool needKey_;
    bool waitingForKey_ = false, keyRequested_ = false;
    std::deque<MediaChunk> q_;
    QueueStats stats_;
    std::vector<LossRange> losses_;
};

class SinkFanout {
public:
    SinkFanout(std::size_t diskCapacityBytes, std::size_t networkCapacityBytes, bool encodedPackets = false);
    // Never blocks. The disk queue always receives the chunk; the network queue only while enabled.
    void publish(const MediaChunk& c);
    void setNetworkEnabled(bool on);         // off => the network queue is emptied and receives nothing
    bool networkEnabled() const { return networkOn_; }
    ConsumerQueue& disk() { return disk_; }
    ConsumerQueue& network() { return net_; }
    std::size_t maxMemoryBytes() const { return disk_.capacity() + net_.capacity(); }   // hard bound on queued payload bytes
private:
    ConsumerQueue disk_, net_;
    bool networkOn_ = false;
};

// ---- encoder sharing decision ---------------------------------------------------------------------------------------
struct EncodeSignature {
    std::string videoCodec;                  // "h264", "mjpeg"
    std::uint32_t width = 0, height = 0, fpsNum = 0, fpsDen = 1;
    std::uint32_t videoBitrateKbps = 0;
    std::string audioCodec;                  // "pcm_s24le", "aac"
    std::uint32_t audioRateHz = 0, audioChannels = 0, audioBitrateKbps = 0;
    bool operator==(const EncodeSignature& o) const {
        return videoCodec == o.videoCodec && width == o.width && height == o.height && fpsNum == o.fpsNum && fpsDen == o.fpsDen &&
               videoBitrateKbps == o.videoBitrateKbps && audioCodec == o.audioCodec && audioRateHz == o.audioRateHz &&
               audioChannels == o.audioChannels && audioBitrateKbps == o.audioBitrateKbps;
    }
};
struct CoexistencePlan {
    bool wouldMatch = false;                 // signatures identical: packet sharing would be legal under section 15
    bool shareEncodedPackets = false;        // always false in this design: two independent ffmpeg processes
    std::uint32_t encoderInstances = 2;      // admission cost to report to the perf module (record + broadcast)
    std::vector<std::string> reasons;
};
CoexistencePlan planCoexistence(const EncodeSignature& record, const EncodeSignature& broadcast);

}  // namespace ss::record
