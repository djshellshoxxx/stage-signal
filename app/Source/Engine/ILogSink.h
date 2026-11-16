// Contract: bounded, sanitised file logging (app-adapters 13, APP-AD-14). Owner: engine-adapters-desktop.
// write() is safe on any non-realtime thread and never blocks long; realtime threads use counters, not logs.
#pragma once
#include <cstdint>
#include <string>

namespace ss::app {

enum class LogLevel : unsigned char { Error, Warn, Info, Debug };

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void write(LogLevel level, const std::string& sanitisedText) = 0;
    virtual std::uint64_t dropped() const = 0;
};

class NullLogSink final : public ILogSink {
public:
    void write(LogLevel, const std::string&) override {}
    std::uint64_t dropped() const override { return 0; }
};

}  // namespace ss::app
