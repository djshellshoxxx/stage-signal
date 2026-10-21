// Test doubles for the record module: in-memory file system and journal sink.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "ss/record/journal.hpp"
#include "ss/record/wav.hpp"

namespace fake {

struct MemFile {
    std::string data;
    std::size_t durable = 0;      // bytes guaranteed after the last flushDurable()
    bool closed = false;
};

class MemFs final : public ss::record::IRecorderFs, public ss::record::IRecoveryFs {
public:
    std::map<std::string, MemFile> files;
    bool failWrites = false;                 // simulate disk full
    std::size_t failAfterBytes = ~std::size_t(0);
    std::size_t written = 0;

    class File final : public ss::record::ISegmentFile {
    public:
        File(MemFs& fs, std::string name) : fs_(fs), name_(std::move(name)) {}
        ss::Status write(const void* d, std::size_t n) override {
            if (fs_.failWrites || fs_.written + n > fs_.failAfterBytes)
                return ss::makeError(ss::ErrorCategory::DiskFull, "disk full");
            fs_.files[name_].data.append(static_cast<const char*>(d), n);
            fs_.written += n;
            return ss::okStatus();
        }
        ss::Status patch(std::uint64_t off, const void* d, std::size_t n) override {
            auto& f = fs_.files[name_];
            if (off + n > f.data.size()) return ss::makeError(ss::ErrorCategory::InternalFault, "patch out of range");
            f.data.replace(off, n, static_cast<const char*>(d), n);
            return ss::okStatus();
        }
        ss::Status flushDurable() override { auto& f = fs_.files[name_]; f.durable = f.data.size(); return ss::okStatus(); }
        ss::Status close() override { auto& f = fs_.files[name_]; f.durable = f.data.size(); f.closed = true; return ss::okStatus(); }
    private:
        MemFs& fs_;
        std::string name_;
    };

    ss::Result<std::unique_ptr<ss::record::ISegmentFile>> createFile(const std::string& name) override {
        files[name] = MemFile{};
        return std::unique_ptr<ss::record::ISegmentFile>(new File(*this, name));
    }
    // Power loss: everything not flushed durably disappears; open files are left as they were at the last flush.
    void crash() {
        for (auto& kv : files)
            if (!kv.second.closed) kv.second.data.resize(kv.second.durable);
    }
    bool fileSize(const std::string& name, std::uint64_t& size) override {
        auto it = files.find(name);
        if (it == files.end()) return false;
        size = it->second.data.size();
        return true;
    }
    ss::Result<std::string> readPrefix(const std::string& name, std::size_t n) override {
        auto it = files.find(name);
        if (it == files.end()) return ss::makeError(ss::ErrorCategory::InvalidArgument, "absent");
        return it->second.data.substr(0, n);
    }
    ss::Status patch(const std::string& name, std::uint64_t off, const std::string& bytes) override {
        auto it = files.find(name);
        if (it == files.end() || off + bytes.size() > it->second.data.size()) return ss::makeError(ss::ErrorCategory::InvalidArgument, "bad patch");
        it->second.data.replace(off, bytes.size(), bytes);
        return ss::okStatus();
    }
    ss::Status truncate(const std::string& name, std::uint64_t newSize) override {
        auto it = files.find(name);
        if (it == files.end()) return ss::makeError(ss::ErrorCategory::InvalidArgument, "absent");
        it->second.data.resize(newSize);
        return ss::okStatus();
    }
};

class MemJournal final : public ss::record::IJournalSink {
public:
    std::vector<std::string> lines;
    bool fail = false;
    ss::Status appendLine(const std::string& l) override {
        if (fail) return ss::makeError(ss::ErrorCategory::DiskFull, "journal full");
        lines.push_back(l);
        return ss::okStatus();
    }
    std::string text() const { std::string s; for (auto& l : lines) s += l + "\n"; return s; }
};

}  // namespace fake
