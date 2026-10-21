#include "ss/record/fanout.hpp"
#include <algorithm>

namespace ss::record {

ConsumerQueue::ConsumerQueue(std::string name, std::size_t cap, OverflowPolicy p, bool needKey)
    : name_(std::move(name)), cap_(cap), policy_(p), needKey_(needKey) {}

void ConsumerQueue::noteLoss(const MediaChunk& c) {
    ++stats_.dropped;
    stats_.droppedBytes += c.size();
    if (!losses_.empty() && losses_.back().fromNs + losses_.back().lengthNs >= c.ptsNs)
        losses_.back().lengthNs = std::max(losses_.back().lengthNs, c.ptsNs + c.durationNs - losses_.back().fromNs);
    else
        losses_.push_back({c.ptsNs, c.durationNs});
    if (needKey_) { waitingForKey_ = true; keyRequested_ = true; }
}

bool ConsumerQueue::push(const MediaChunk& c) {
    if (c.size() > cap_) { noteLoss(c); return false; }               // a single chunk larger than the queue never fits
    if (waitingForKey_) {
        if (!c.keyframe) { noteLoss(c); return false; }
        waitingForKey_ = false;
    }
    if (stats_.depthBytes + c.size() > cap_) {
        if (policy_ == OverflowPolicy::DropNewest) { noteLoss(c); return false; }
        while (!q_.empty() && stats_.depthBytes + c.size() > cap_) {   // DropOldest
            stats_.depthBytes -= q_.front().size();
            noteLoss(q_.front());
            q_.pop_front();
        }
        if (needKey_) {                                                // do not start the consumer mid-GOP
            while (!q_.empty() && !q_.front().keyframe) { stats_.depthBytes -= q_.front().size(); noteLoss(q_.front()); q_.pop_front(); }
            if (!c.keyframe && q_.empty()) { noteLoss(c); return false; }
        }
    }
    q_.push_back(c);
    stats_.depthBytes += c.size();
    stats_.maxDepthBytes = std::max(stats_.maxDepthBytes, stats_.depthBytes);
    ++stats_.pushed;
    return true;
}

bool ConsumerQueue::pop(MediaChunk& out) {
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    stats_.depthBytes -= out.size();
    ++stats_.popped;
    return true;
}

void ConsumerQueue::clear() {
    q_.clear();
    stats_.depthBytes = 0;
    if (needKey_) waitingForKey_ = true;       // abandon queued media; resume at a keyframe
}

bool ConsumerQueue::takeKeyframeRequest() { const bool r = keyRequested_; keyRequested_ = false; return r; }

std::vector<LossRange> ConsumerQueue::takeLosses() { std::vector<LossRange> v; v.swap(losses_); return v; }

SinkFanout::SinkFanout(std::size_t diskCap, std::size_t netCap, bool encoded)
    : disk_("disk", diskCap, OverflowPolicy::DropNewest, false), net_("network", netCap, OverflowPolicy::DropOldest, encoded) {}

void SinkFanout::publish(const MediaChunk& c) {
    disk_.push(c);                              // first, and independent of the network outcome
    if (networkOn_) net_.push(c);
}

void SinkFanout::setNetworkEnabled(bool on) {
    if (networkOn_ && !on) net_.clear();
    networkOn_ = on;
}

CoexistencePlan planCoexistence(const EncodeSignature& rec, const EncodeSignature& cast) {
    CoexistencePlan p;
    p.wouldMatch = rec == cast;
    p.shareEncodedPackets = false;
    p.encoderInstances = 2;
    if (!p.wouldMatch) {
        if (rec.audioCodec != cast.audioCodec) p.reasons.push_back("recording keeps lossless PCM audio, the stream uses AAC: audio cannot be shared");
        if (rec.videoCodec != cast.videoCodec || rec.width != cast.width || rec.height != cast.height || rec.fpsNum != cast.fpsNum || rec.fpsDen != cast.fpsDen)
            p.reasons.push_back("video codec, size or frame rate differs: a second encoder instance is required");
        if (rec.videoBitrateKbps != cast.videoBitrateKbps) p.reasons.push_back("video bitrate differs");
    } else {
        p.reasons.push_back("settings are identical, but this build runs two independent ffmpeg processes so a slow network can never stall the file muxer");
    }
    p.reasons.push_back("two encoder instances must pass performance admission");
    return p;
}

}  // namespace ss::record
