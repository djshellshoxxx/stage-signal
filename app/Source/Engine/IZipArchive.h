// Contract: safe ZIP read/write (app-adapters 12, APP-AD-12). Owner: engine-adapters-desktop.
// Reading is two-phase: list headers only, validate with security::validateArchive, then extract member by member
// charging an ExtractionBudget. Writing uses fixed member order and fixed timestamps.
#pragma once
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/security/archive.hpp"

namespace ss::app {

struct ZipMember { std::string name; std::string bytes; };

class IZipArchive {
public:
    virtual ~IZipArchive() = default;
    virtual ss::Result<std::vector<ss::security::ArchiveEntryInfo>> list(const std::string& zipPath) = 0;
    virtual ss::Result<std::string> readMember(const std::string& zipPath, std::size_t index, std::uint64_t maxBytes) = 0;
    virtual ss::Status write(const std::string& zipPath, const std::vector<ZipMember>& members) = 0;   // atomic (tmp + rename)
};

class UnsupportedZipArchive final : public IZipArchive {
public:
    ss::Result<std::vector<ss::security::ArchiveEntryInfo>> list(const std::string&) override { return unsupported(); }
    ss::Result<std::string> readMember(const std::string&, std::size_t, std::uint64_t) override { return unsupported(); }
    ss::Status write(const std::string&, const std::vector<ZipMember>&) override { return unsupported(); }
private:
    static ss::Error unsupported() {
        return ss::makeError(ss::ErrorCategory::UnsupportedCapability, "zip adapter not available");
    }
};

}  // namespace ss::app
