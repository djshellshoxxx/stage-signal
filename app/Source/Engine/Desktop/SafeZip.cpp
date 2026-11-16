// Safe ZIP (app-adapters 12, APP-AD-12). The central directory is parsed here (JUCE's ZipEntry lacks compressed size,
// attributes and encryption flags) and validated by security::validateArchive; juce::ZipFile only inflates, and its
// view of the entry (count, name, size) must agree with ours or the member is refused (parser-differential guard).
#include <algorithm>
#include <memory>
#include <juce_core/juce_core.h>
#include "Engine/Desktop/DesktopCommon.h"
#include "Engine/Desktop/DesktopSystem.h"

namespace ss::app {

namespace {
using Entries = std::vector<ss::security::ArchiveEntryInfo>;
std::uint32_t rd16(const std::string& b, std::size_t o) { return static_cast<std::uint8_t>(b[o]) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[o + 1])) << 8); }
std::uint32_t rd32(const std::string& b, std::size_t o) { return rd16(b, o) | (rd16(b, o + 2) << 16); }

ss::Error bad(const char* why) { return desktop::err(ss::ErrorCategory::InvalidArgument, std::string("zip rejected: ") + why); }

ss::Result<Entries> parseCentral(const std::string& zipPath) {
    juce::FileInputStream in(juce::File(juce::String::fromUTF8(zipPath.c_str())));
    if (in.failedToOpen()) return desktop::err(ss::ErrorCategory::InvalidArgument, "zip cannot be opened");
    const std::int64_t size = in.getTotalLength();
    if (size < 22) return bad("too small");
    const std::int64_t tail = std::min<std::int64_t>(size, 22 + 65535);
    std::string t(static_cast<std::size_t>(tail), '\0');
    in.setPosition(size - tail);
    if (in.read(&t[0], static_cast<int>(tail)) != static_cast<int>(tail)) return bad("read error");
    std::int64_t eocd = -1;
    for (std::int64_t i = tail - 22; i >= 0; --i)
        if (rd32(t, static_cast<std::size_t>(i)) == 0x06054b50u) { eocd = i; break; }
    if (eocd < 0) return bad("no end record");
    const auto e = static_cast<std::size_t>(eocd);
    const std::uint32_t total = rd16(t, e + 10), cdSize = rd32(t, e + 12), cdOff = rd32(t, e + 16);
    if (rd16(t, e + 4) != 0 || rd16(t, e + 6) != 0 || rd16(t, e + 8) != total) return bad("multi-disk");
    if (total == 0xFFFFu || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) return bad("zip64 unsupported");
    if (total > 10000) return bad("too many entries");
    if (cdSize > (16u << 20) || static_cast<std::int64_t>(cdOff) + cdSize > size) return bad("bad directory");
    std::string cd(cdSize, '\0');
    in.setPosition(cdOff);
    if (cdSize && in.read(&cd[0], static_cast<int>(cdSize)) != static_cast<int>(cdSize)) return bad("read error");
    Entries out;
    std::size_t o = 0;
    for (std::uint32_t i = 0; i < total; ++i) {
        if (o + 46 > cd.size() || rd32(cd, o) != 0x02014b50u) return bad("bad entry");
        const std::uint32_t made = rd16(cd, o + 4), flags = rd16(cd, o + 8), method = rd16(cd, o + 10);
        const std::uint32_t csz = rd32(cd, o + 20), usz = rd32(cd, o + 24);
        const std::size_t nl = rd16(cd, o + 28), xl = rd16(cd, o + 30), cl = rd16(cd, o + 32);
        const std::uint32_t ext = rd32(cd, o + 38);
        if (o + 46 + nl + xl + cl > cd.size()) return bad("bad entry length");
        if (flags & 1u) return bad("encrypted entry");
        if (method != 0 && method != 8) return bad("unsupported compression");
        if (csz == 0xFFFFFFFFu || usz == 0xFFFFFFFFu) return bad("zip64 unsupported");
        ss::security::ArchiveEntryInfo info;
        info.path.assign(cd, o + 46, nl);
        info.uncompressedSize = usz; info.compressedSize = csz;
        info.isDirectory = (!info.path.empty() && info.path.back() == '/') || (ext & 0x10u);
        const std::uint32_t host = made >> 8;
        info.isSymlinkOrReparse = (host == 3 && (((ext >> 16) & 0xF000u) == 0xA000u)) || (host == 0 && (ext & 0x400u));
        out.push_back(std::move(info));
        o += 46 + nl + xl + cl;
    }
    std::uint64_t tu = 0, tc = 0;
    for (const auto& x : out) { tu += x.uncompressedSize; tc += x.compressedSize; }
    auto hs = ss::security::checkArchiveHeader(out.size(), tu, tc);
    if (!hs.ok()) return hs.error();
    return out;
}
}  // namespace

ss::Result<Entries> SafeZipArchive::list(const std::string& zipPath) { return parseCentral(zipPath); }

ss::Result<std::string> SafeZipArchive::readMember(const std::string& zipPath, std::size_t index, std::uint64_t maxBytes) {
    auto listing = parseCentral(zipPath);
    if (!listing.ok()) return listing.error();
    const Entries& es = listing.value();
    auto validated = ss::security::validateArchive(es);
    if (!validated.ok()) return validated.error();
    if (index >= es.size()) return desktop::err(ss::ErrorCategory::InvalidArgument, "zip member index out of range");
    const auto& e = es[index];
    if (e.isDirectory) return desktop::err(ss::ErrorCategory::InvalidArgument, "zip member is a directory");
    if (e.uncompressedSize > maxBytes) return desktop::err(ss::ErrorCategory::ResourceExhausted, "zip member too large");
    juce::ZipFile zf(juce::File(juce::String::fromUTF8(zipPath.c_str())));
    if (static_cast<std::size_t>(zf.getNumEntries()) != es.size()) return bad("directory mismatch");
    const auto* je = zf.getEntry(static_cast<int>(index));
    if (!je || je->filename != juce::String::fromUTF8(e.path.c_str()) || static_cast<std::uint64_t>(je->uncompressedSize) != e.uncompressedSize)
        return bad("entry mismatch");
    std::unique_ptr<juce::InputStream> in(zf.createStreamForEntry(static_cast<int>(index)));
    if (!in) return bad("cannot inflate");
    std::string out;
    out.reserve(static_cast<std::size_t>(e.uncompressedSize));
    char buf[65536];
    for (;;) {
        const int n = in->read(buf, sizeof(buf));
        if (n < 0) return bad("read error");
        if (n == 0) break;
        if (out.size() + static_cast<std::size_t>(n) > e.uncompressedSize || out.size() + static_cast<std::size_t>(n) > maxBytes)
            return desktop::err(ss::ErrorCategory::ResourceExhausted, "zip member exceeds declared size");
        out.append(buf, static_cast<std::size_t>(n));
    }
    if (out.size() != e.uncompressedSize) return bad("size mismatch");
    return out;
}

ss::Status SafeZipArchive::write(const std::string& zipPath, const std::vector<ZipMember>& members) {
    if (members.size() > 10000) return bad("too many entries");
    std::vector<std::string> folded;
    juce::ZipFile::Builder builder;
    for (const auto& m : members) {
        auto c = ss::security::canonicalMemberPath(m.name);
        if (!c.ok() || c.value() != m.name) return bad("member name not canonical");
        std::string f = m.name;
        std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (std::find(folded.begin(), folded.end(), f) != folded.end()) return bad("duplicate member");
        folded.push_back(f);
        if (m.bytes.size() > (1u << 30)) return desktop::err(ss::ErrorCategory::ResourceExhausted, "member too large");
        builder.addEntry(std::make_unique<juce::MemoryInputStream>(m.bytes.data(), m.bytes.size(), true), 5, juce::String(m.name),
                         juce::Time(1980, 0, 1, 0, 0, 0, 0, true));    // fixed timestamp: deterministic output
    }
    const std::string tmp = zipPath + ".tmp";
    const juce::File tf(juce::String::fromUTF8(tmp.c_str()));
    tf.deleteFile();
    {
        juce::FileOutputStream os(tf);
        if (os.failedToOpen()) return desktop::err(ss::ErrorCategory::DiskFull, "cannot create zip file");
        if (!builder.writeToStream(os, nullptr)) { os.flush(); tf.deleteFile(); return desktop::err(ss::ErrorCategory::DiskFull, "zip write failed"); }
        os.flush();
    }
    FsyncDurableFileSystem fs;
    if (auto s = fs.syncFile(tmp); !s.ok()) { tf.deleteFile(); return s; }
    if (auto s = fs.rename(tmp, zipPath); !s.ok()) { tf.deleteFile(); return s; }
    const auto slash = zipPath.find_last_of('/');
    if (slash != std::string::npos) (void) fs.syncDirectory(zipPath.substr(0, slash == 0 ? 1 : slash));
    return ss::okStatus();
}

}  // namespace ss::app
