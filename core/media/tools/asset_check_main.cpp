// ss_asset_check: packaging gate for REQ-S15.
//   ss_asset_check --manifest <assets.json> --root <asset dir> [--allow-unlisted]
// Exit codes: 0 all entries verified, 1 absent/mismatched/incomplete entry, 2 usage or manifest parse error.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include "ss/base/sha256.hpp"
#include "ss/media/manifest.hpp"

namespace fs = std::filesystem;
using namespace ss;
using namespace ss::media;

namespace {

class FsAssetSource final : public IAssetSource {
public:
    explicit FsAssetSource(fs::path root) : root_(std::move(root)) {}
    Result<std::string> sha256Hex(const std::string& rel, std::uint64_t* sizeOut) override {
        std::error_code ec;
        const fs::path p = root_ / fs::u8path(rel);
        if (fs::is_symlink(p, ec) || !fs::is_regular_file(p, ec))
            return makeError(ErrorCategory::InvalidArgument, "not a regular file");
        std::ifstream in(p, std::ios::binary);
        if (!in) return makeError(ErrorCategory::PermissionDenied, "cannot open file");
        Sha256 h;
        std::vector<char> buf(1 << 16);
        std::uint64_t total = 0;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto got = in.gcount();
            if (got > 0) { h.update(buf.data(), static_cast<std::size_t>(got)); total += static_cast<std::uint64_t>(got); }
        }
        if (in.bad()) return makeError(ErrorCategory::InternalFault, "read error");
        if (sizeOut) *sizeOut = total;
        return Sha256::hex(h.finish());
    }
    std::vector<std::string> listFiles() override {
        std::vector<std::string> out;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_symlink(ec) || !it->is_regular_file(ec)) continue;
            out.push_back(fs::relative(it->path(), root_, ec).generic_u8string());
        }
        return out;
    }
private:
    fs::path root_;
};

int usage() {
    std::fprintf(stderr, "usage: ss_asset_check --manifest <assets.json> --root <dir> [--allow-unlisted]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string manifestPath, rootPath;
    bool allowUnlisted = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--manifest" && i + 1 < argc) manifestPath = argv[++i];
        else if (a == "--root" && i + 1 < argc) rootPath = argv[++i];
        else if (a == "--allow-unlisted") allowUnlisted = true;
        else return usage();
    }
    if (manifestPath.empty() || rootPath.empty()) return usage();

    std::ifstream in(manifestPath, std::ios::binary);
    if (!in) { std::fprintf(stderr, "error: cannot read manifest\n"); return 2; }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto parsed = parseManifestJson(text);
    if (!parsed.ok()) { std::fprintf(stderr, "error: %s\n", parsed.error().detail.c_str()); return 2; }

    std::error_code ec;
    if (!fs::is_directory(rootPath, ec)) { std::fprintf(stderr, "error: asset root is not a directory\n"); return 2; }

    FsAssetSource src{fs::path(rootPath)};
    VerifyOptions opt;
    opt.requireAllFilesListed = !allowUnlisted;
    const fs::path mp = fs::weakly_canonical(fs::path(manifestPath), ec);
    const fs::path rp = fs::weakly_canonical(fs::path(rootPath), ec);
    const fs::path rel = mp.lexically_relative(rp);
    if (!rel.empty() && *rel.begin() != "..") opt.ignorePaths.insert(rel.generic_u8string());   // manifest inside root

    const VerifyReport rep = verifyManifest(parsed.value(), src, opt);
    for (const auto& f : rep.findings)
        std::fprintf(stderr, "FAIL %s [%s] %s\n", f.assetId.empty() ? "-" : f.assetId.c_str(), f.field.c_str(), f.message.c_str());
    std::printf("%zu entries checked, %zu finding(s): %s\n", rep.entriesChecked, rep.findings.size(), rep.ok() ? "PASS" : "FAIL");
    return rep.ok() ? 0 : 1;
}
