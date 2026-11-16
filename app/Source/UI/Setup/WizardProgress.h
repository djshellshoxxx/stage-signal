#pragma once
#include <array>
#include <string>

namespace ss::app::setup {
struct WizardProgress {
    int step = 0;
    bool completed = false, audioSeen = false, audioSkipped = false;
    std::array<unsigned char, 8> results{};
    std::string encode() const {
        std::string out;
        out += char('0' + step); out += completed ? '1' : '0'; out += audioSeen ? '1' : '0';
        out += audioSkipped ? '1' : '0'; out += ':';
        for (auto result : results) out += char('0' + result);
        return out;
    }
    static bool decode(const std::string& value, WizardProgress& out) {
        if (value.size() != 13 || value[0] < '0' || value[0] > '7' || value[4] != ':') return false;
        for (int i = 1; i < 4; ++i) if (value[std::size_t(i)] != '0' && value[std::size_t(i)] != '1') return false;
        for (int i = 5; i < 13; ++i) if (value[std::size_t(i)] < '0' || value[std::size_t(i)] > '2') return false;
        WizardProgress parsed;
        parsed.step = value[0] - '0'; parsed.completed = value[1] == '1';
        parsed.audioSeen = value[2] == '1'; parsed.audioSkipped = value[3] == '1';
        for (int i = 0; i < 8; ++i) parsed.results[std::size_t(i)] = static_cast<unsigned char>(value[std::size_t(i + 5)] - '0');
        if (parsed.completed && parsed.step != 7) return false;
        out = parsed; return true;
    }
};
}  // namespace ss::app::setup
