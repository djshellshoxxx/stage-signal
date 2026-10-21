// StageSignal base: typed results (spec section 8/10: error categories, recoverability).
#pragma once
#include <string>
#include <utility>
#include <variant>
#include <cstdint>

namespace ss {

enum class ErrorCategory : std::uint8_t {
    InvalidArgument, UnsupportedCapability, PermissionDenied, DeviceLost,
    TimestampInvalid, ResourceExhausted, DecodeFailure, EncoderUnavailable,
    DiskFull, TransportFailure, SchemaUnsupported, Cancelled, InternalFault
};

// A recovery recommendation is a named operator action, never "retry forever".
enum class Recoverability : std::uint8_t { Retryable, OperatorAction, Fatal };

inline const char* categoryName(ErrorCategory c) {
    switch (c) {
        case ErrorCategory::InvalidArgument: return "InvalidArgument";
        case ErrorCategory::UnsupportedCapability: return "UnsupportedCapability";
        case ErrorCategory::PermissionDenied: return "PermissionDenied";
        case ErrorCategory::DeviceLost: return "DeviceLost";
        case ErrorCategory::TimestampInvalid: return "TimestampInvalid";
        case ErrorCategory::ResourceExhausted: return "ResourceExhausted";
        case ErrorCategory::DecodeFailure: return "DecodeFailure";
        case ErrorCategory::EncoderUnavailable: return "EncoderUnavailable";
        case ErrorCategory::DiskFull: return "DiskFull";
        case ErrorCategory::TransportFailure: return "TransportFailure";
        case ErrorCategory::SchemaUnsupported: return "SchemaUnsupported";
        case ErrorCategory::Cancelled: return "Cancelled";
        case ErrorCategory::InternalFault: return "InternalFault";
    }
    return "Unknown";
}

struct Error {
    ErrorCategory category = ErrorCategory::InternalFault;
    Recoverability recoverability = Recoverability::OperatorAction;
    std::string detail;            // sanitized: never contains secrets or user paths
    std::uint64_t revision = 0;    // originating show revision, 0 when not applicable
};

inline Error makeError(ErrorCategory c, std::string detail,
                       Recoverability r = Recoverability::OperatorAction, std::uint64_t rev = 0) {
    return Error{c, r, std::move(detail), rev};
}

struct Unit {};

template <class T>
class Result {
public:
    Result(T v) : v_(std::move(v)) {}                 // NOLINT implicit by design
    Result(Error e) : v_(std::move(e)) {}             // NOLINT implicit by design
    bool ok() const { return std::holds_alternative<T>(v_); }
    explicit operator bool() const { return ok(); }
    const T& value() const { return std::get<T>(v_); }
    T& value() { return std::get<T>(v_); }
    const Error& error() const { return std::get<Error>(v_); }
    T valueOr(T fallback) const { return ok() ? value() : std::move(fallback); }
private:
    std::variant<T, Error> v_;
};

using Status = Result<Unit>;
inline Status okStatus() { return Status(Unit{}); }

}  // namespace ss
