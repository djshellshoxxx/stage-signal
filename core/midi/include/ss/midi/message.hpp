// StageSignal midi: message model, byte-stream parser and normaliser (spec section 14).
// Note-on with velocity 0 is normalised to note-off. Running status and interleaved realtime bytes are handled.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "ss/base/time.hpp"

namespace ss::midi {

enum class MsgKind : std::uint8_t { NoteOff, NoteOn, ControlChange, ProgramChange, Clock, Start, Continue, Stop, SysEx };

struct Message {
    MsgKind kind = MsgKind::NoteOff;
    std::uint8_t channel = 0;           // 0..15 for channel messages
    std::uint8_t data1 = 0;             // note / controller / program
    std::uint8_t data2 = 0;             // velocity / value (0 for note-off, program change, realtime)
    bool fromZeroVelocity = false;      // NoteOff produced from NoteOn velocity 0
    Ns timestamp = 0;
    std::vector<std::uint8_t> sysex;    // full F0..F7 frame, only for kind == SysEx

    bool isRealtime() const {
        return kind == MsgKind::Clock || kind == MsgKind::Start || kind == MsgKind::Continue || kind == MsgKind::Stop;
    }
    bool isChannelMessage() const { return !isRealtime() && kind != MsgKind::SysEx; }
    bool operator==(const Message& o) const {
        return kind == o.kind && channel == o.channel && data1 == o.data1 && data2 == o.data2 && sysex == o.sysex;
    }
};

struct ParserStats {
    std::uint64_t messages = 0, malformedBytes = 0, unsupportedMessages = 0, ignoredRealtime = 0;
    std::uint64_t sysexAccepted = 0, sysexBlocked = 0, sysexOversize = 0, sysexAborted = 0;
};

class Parser {
public:
    explicit Parser(bool sysexEnabled = false) : sysexEnabled_(sysexEnabled) {}
    void setSysexEnabled(bool on) { sysexEnabled_ = on; }
    bool sysexEnabled() const { return sysexEnabled_; }
    void reset();                                              // new connection generation: forget running status
    void feed(const std::uint8_t* bytes, std::size_t n, Ns timestamp, std::vector<Message>& out);
    const ParserStats& stats() const { return stats_; }
private:
    void emitChannel(Ns ts, std::vector<Message>& out);
    bool sysexEnabled_;
    std::uint8_t running_ = 0, need_ = 0, idx_ = 0, swallow_ = 0, data_[2] = {0, 0};
    bool inSysex_ = false, sysexSkip_ = false;
    std::vector<std::uint8_t> sysexBuf_;
    ParserStats stats_;
};

// Convenience: parse one complete packet (as JUCE/RtMidi deliver them) with a fresh parser.
std::vector<Message> parsePacket(const std::uint8_t* bytes, std::size_t n, bool sysexEnabled = false, Ns ts = 0);

// Wire encoding of channel/realtime messages (not SysEx). Returns byte count (1..3), 0 when not encodable here.
std::size_t encode(const Message& m, std::uint8_t out[3]);

Message makeNoteOn(std::uint8_t ch, std::uint8_t note, std::uint8_t vel);
Message makeNoteOff(std::uint8_t ch, std::uint8_t note);
Message makeCC(std::uint8_t ch, std::uint8_t cc, std::uint8_t value);
Message makeProgramChange(std::uint8_t ch, std::uint8_t program);
Message makeRealtime(MsgKind kind);

}  // namespace ss::midi
