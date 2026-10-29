// Shared helpers for ui-other (app-ui 3.5 copy, UiModel calls). Owner: ui-other. Header only, JUCE-free logic.
// Every UI write goes through here: UiModel::submitAction for actions, UiModel::postCommand for AppCommands.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <cmath>
#include <juce_graphics/juce_graphics.h>
#include "App/UiModel.h"
#include "UI/Theme.h"
#include "ss/palette/registry.hpp"

namespace ss::app::overlay {

// Number of Unicode code points in UTF-8 text (counts lead bytes only). Used for the 4096-character limit.
inline std::size_t codePointCount(const std::string& utf8) {
    std::size_t n = 0;
    for (const char ch : utf8)
        if ((static_cast<unsigned int>(static_cast<unsigned char>(ch)) & 0xC0u) != 0x80u) ++n;
    return n;
}

// Body text font (app-ui 3.3). Font(float) is deprecated in JUCE 8, so every panel goes through here.
inline juce::Font bodyFont(float height = theme::kBodyFont) { return juce::Font(juce::FontOptions(height)); }

inline const char* adapterWord(AdapterState s) {
    switch (s) {
        case AdapterState::Stopped: return "Stopped";
        case AdapterState::Starting: return "Starting";
        case AdapterState::Running: return "Running";
        case AdapterState::Degraded: return "Degraded";
        case AdapterState::Fault: return "Fault";
        case AdapterState::Unavailable: return "Unavailable";
    }
    return "Unknown";
}

// Output label from the snapshot, or the plain name for "all visual outputs".
inline std::string outputLabel(const UiSnapshot& s, OutputId id) {
    if (id == kAllOutputs) return "All visual outputs";
    for (const auto& o : s.outputs)
        if (o.id == id) return o.label;
    return "Selected output";
}

// Sends an action through the one queue used by buttons and the palette. Returns the operator-facing line.
// `correlationOut` receives the id to match against ActionOutcome.
inline std::string sendAction(UiModel& m, ActionId id, double value, OutputId target, std::uint64_t* correlationOut) {
    ss::Action a;
    a.actionId = ss::id(id);
    a.source = ss::ActionSource::Ui;
    a.target = target;
    a.value = value;
    const auto ticket = m.submitAction(a);
    if (correlationOut) *correlationOut = ticket.correlationId;
    if (!ticket.result.queued) return ticket.result.reason.empty() ? "Not sent. Try again." : ticket.result.reason;
    return "Sent.";
}

// Posts a non-action request for the IO thread. Returns false when the command queue is full.
inline bool sendCommand(UiModel& m, AppCommand c, std::uint64_t* correlationOut = nullptr) {
    return m.postCommand(std::move(c), correlationOut);
}

// Plain number text: integers without decimals, otherwise up to 2 decimals.
inline std::string numberText(double v) {
    char buf[32];
    if (std::fabs(v - std::round(v)) < 1e-9) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    else std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

// "Seconds ago" text for an event time against the snapshot clock.
inline std::string agoText(ss::Ns nowNs, ss::Ns thenNs) {
    if (thenNs <= 0 || nowNs < thenNs) return "just now";
    const auto secs = (nowNs - thenNs) / ss::kNsPerSec;
    if (secs < 60) return std::to_string(secs) + " s ago";
    return std::to_string(secs / 60) + " min ago";
}

}  // namespace ss::app::overlay
