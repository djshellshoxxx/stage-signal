#include "ss/midi/message.hpp"
#include "ss/midi/limits.hpp"

namespace ss::midi {

void Parser::reset() {
    running_ = 0; need_ = 0; idx_ = 0; swallow_ = 0; inSysex_ = false; sysexSkip_ = false; sysexBuf_.clear();
}

void Parser::emitChannel(Ns ts, std::vector<Message>& out) {
    Message m;
    m.timestamp = ts;
    m.channel = running_ & 0x0F;
    m.data1 = data_[0];
    m.data2 = need_ == 2 ? data_[1] : 0;
    switch (running_ & 0xF0) {
        case 0x80: m.kind = MsgKind::NoteOff; m.data2 = 0; break;      // release velocity is not used
        case 0x90:
            if (data_[1] == 0) { m.kind = MsgKind::NoteOff; m.data2 = 0; m.fromZeroVelocity = true; }
            else m.kind = MsgKind::NoteOn;
            break;
        case 0xB0: m.kind = MsgKind::ControlChange; break;
        case 0xC0: m.kind = MsgKind::ProgramChange; break;
        default: ++stats_.unsupportedMessages; return;                   // poly/channel pressure, pitch bend
    }
    ++stats_.messages;
    out.push_back(std::move(m));
}

void Parser::feed(const std::uint8_t* bytes, std::size_t n, Ns ts, std::vector<Message>& out) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t b = bytes[i];
        if (b >= 0xF8) {                                    // realtime: may appear anywhere, never alters state
            Message m;
            m.timestamp = ts;
            switch (b) {
                case 0xF8: m.kind = MsgKind::Clock; break;
                case 0xFA: m.kind = MsgKind::Start; break;
                case 0xFB: m.kind = MsgKind::Continue; break;
                case 0xFC: m.kind = MsgKind::Stop; break;
                default: ++stats_.ignoredRealtime; continue;       // active sensing, reset, undefined
            }
            ++stats_.messages;
            out.push_back(std::move(m));
            continue;
        }
        if (b & 0x80) {                                      // status byte
            if (inSysex_) {
                if (b == 0xF7) {
                    inSysex_ = false;
                    if (sysexSkip_) { sysexSkip_ = false; ++stats_.sysexBlocked; }
                    else {
                        sysexBuf_.push_back(0xF7);
                        if (sysexBuf_.size() > kMaxSysexBytes) { ++stats_.sysexOversize; }
                        else {
                            Message m; m.kind = MsgKind::SysEx; m.timestamp = ts; m.sysex = std::move(sysexBuf_);
                            ++stats_.sysexAccepted; ++stats_.messages; out.push_back(std::move(m));
                        }
                    }
                    sysexBuf_.clear();
                    continue;
                }
                inSysex_ = false; sysexSkip_ = false; sysexBuf_.clear();   // any other status aborts the frame
                ++stats_.sysexAborted;
            }
            if (b == 0xF0) {
                inSysex_ = true; running_ = 0; idx_ = 0; swallow_ = 0;
                sysexSkip_ = !sysexEnabled_;
                sysexBuf_.clear();
                if (!sysexSkip_) sysexBuf_.push_back(0xF0);
                continue;
            }
            if (b == 0xF7) { ++stats_.malformedBytes; continue; }       // stray end-of-sysex
            if (b < 0xF0) {
                running_ = b; idx_ = 0; swallow_ = 0;
                const std::uint8_t hi = b & 0xF0;
                need_ = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            } else {                                               // system common: cancels running status
                running_ = 0; idx_ = 0; need_ = 0;
                swallow_ = (b == 0xF1 || b == 0xF3) ? 1 : (b == 0xF2 ? 2 : 0);   // data bytes are swallowed
                ++stats_.unsupportedMessages;
            }
            continue;
        }
        // data byte
        if (inSysex_) {
            if (sysexSkip_) continue;
            if (sysexBuf_.size() < kMaxSysexBytes + 1) sysexBuf_.push_back(b);   // one past the limit marks oversize
            continue;
        }
        if (swallow_ > 0) { --swallow_; continue; }
        if (running_ == 0) { ++stats_.malformedBytes; continue; }
        data_[idx_++] = b;
        if (idx_ == need_) { emitChannel(ts, out); idx_ = 0; }
    }
}

std::vector<Message> parsePacket(const std::uint8_t* bytes, std::size_t n, bool sysexEnabled, Ns ts) {
    Parser p(sysexEnabled);
    std::vector<Message> out;
    p.feed(bytes, n, ts, out);
    return out;
}

std::size_t encode(const Message& m, std::uint8_t out[3]) {
    switch (m.kind) {
        case MsgKind::NoteOff: out[0] = static_cast<std::uint8_t>(0x80 | (m.channel & 0x0F)); out[1] = m.data1 & 0x7F; out[2] = 0; return 3;
        case MsgKind::NoteOn: out[0] = static_cast<std::uint8_t>(0x90 | (m.channel & 0x0F)); out[1] = m.data1 & 0x7F; out[2] = m.data2 & 0x7F; return 3;
        case MsgKind::ControlChange: out[0] = static_cast<std::uint8_t>(0xB0 | (m.channel & 0x0F)); out[1] = m.data1 & 0x7F; out[2] = m.data2 & 0x7F; return 3;
        case MsgKind::ProgramChange: out[0] = static_cast<std::uint8_t>(0xC0 | (m.channel & 0x0F)); out[1] = m.data1 & 0x7F; return 2;
        case MsgKind::Clock: out[0] = 0xF8; return 1;
        case MsgKind::Start: out[0] = 0xFA; return 1;
        case MsgKind::Continue: out[0] = 0xFB; return 1;
        case MsgKind::Stop: out[0] = 0xFC; return 1;
        case MsgKind::SysEx: return 0;
    }
    return 0;
}

static Message mk(MsgKind k, std::uint8_t ch, std::uint8_t d1, std::uint8_t d2) {
    Message m; m.kind = k; m.channel = ch; m.data1 = d1; m.data2 = d2; return m;
}
Message makeNoteOn(std::uint8_t ch, std::uint8_t note, std::uint8_t vel) { return mk(MsgKind::NoteOn, ch, note, vel); }
Message makeNoteOff(std::uint8_t ch, std::uint8_t note) { return mk(MsgKind::NoteOff, ch, note, 0); }
Message makeCC(std::uint8_t ch, std::uint8_t cc, std::uint8_t value) { return mk(MsgKind::ControlChange, ch, cc, value); }
Message makeProgramChange(std::uint8_t ch, std::uint8_t program) { return mk(MsgKind::ProgramChange, ch, program, 0); }
Message makeRealtime(MsgKind kind) { return mk(kind, 0, 0, 0); }

}  // namespace ss::midi
