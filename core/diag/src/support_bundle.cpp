#include "ss/diag/support_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include "ss/base/utf8.hpp"
#include "ss/diag/text.hpp"
#include "ss/security/archive.hpp"

namespace ss::diag {
namespace {

struct CatInfo { const char* label; const char* description; bool defaultOn; bool forbidden; };
const CatInfo kCats[] = {
    {"Application info", "StageSignal version, build identifier and enabled protocol list.", true, false},
    {"System summary", "OS name and version, CPU model and core count, memory size, display and audio device counts. No serial numbers.", true, false},
    {"Event journal", "Timestamped scene, cue, mode, device, recording and broadcast events and markers, with secrets removed.", true, false},
    {"Logs", "Recent application logs (rotated 5 x 5 MiB), with URLs reduced to host and path, usernames and secrets removed.", true, false},
    {"Capabilities", "Measured performance, admitted and rejected features and adaptation steps.", true, false},
    {"Show structure", "Scene, cue, output and mapping records without media files or text content.", false, false},
    {"Settings (redacted)", "Local settings with device identities shortened and secret references only.", false, false},
    {"Show report", "The HTML/CSV show report for the last session.", false, false},
    {"Raw audio", "Never included.", false, true},
    {"Camera frames", "Never included.", false, true},
    {"Key material", "Stream keys, passwords and certificates are never included.", false, true},
    {"Full media", "Show media files are never included.", false, true},
    {"Crash dumps", "Memory dumps are never included.", false, true},
};
static_assert(sizeof(kCats) / sizeof(kCats[0]) == static_cast<std::size_t>(BundleCategory::Count), "category table out of sync");

const CatInfo& info(BundleCategory c) { return kCats[static_cast<std::size_t>(c)]; }

Error denied(const char* why) { return makeError(ErrorCategory::PermissionDenied, std::string("support bundle: ") + why); }
Error bad(const char* why) { return makeError(ErrorCategory::InvalidArgument, std::string("support bundle: ") + why); }

bool forbiddenExtension(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* deny[] = {"wav", "flac", "mp3", "ogg", "aif", "aiff", "m4a", "opus", "mp4", "mkv", "mov", "avi", "webm", "png", "jpg", "jpeg", "bmp",
        "gif", "tif", "tiff", "dmp", "mdmp", "core", "pem", "key", "p12", "pfx", "keystore", "kdbx", "crt", "cer", "der", "zip", "7z", "rar"};
    for (const char* d : deny) if (ext == d) return true;
    return false;
}

bool looksBinaryOrKey(const std::string& s, const char*& why) {
    if (s.find('\0') != std::string::npos) { why = "binary content"; return true; }
    if (!utf8::isValid(s)) { why = "content is not valid UTF-8 text"; return true; }
    if (s.find("-----BEGIN") != std::string::npos) { why = "key or certificate material"; return true; }
    if (s.compare(0, 4, "RIFF") == 0 || s.compare(0, 4, "fLaC") == 0 || s.compare(0, 4, "OggS") == 0 || s.compare(0, 4, "\x89PNG") == 0 ||
        s.compare(0, 3, "\xFF\xD8\xFF") == 0 || s.compare(0, 2, "PK") == 0) { why = "binary media or archive content"; return true; }
    return false;
}

std::vector<std::string> firstLines(const std::string& s, std::size_t n) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size() && out.size() < n) {
        std::size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        out.push_back(security::truncateUtf8(s.substr(i, e - i), 120));
        i = e + 1;
    }
    return out;
}

}  // namespace

bool isForbidden(BundleCategory c) { return info(c).forbidden; }
bool defaultSelected(BundleCategory c) { return info(c).defaultOn; }
const char* categoryLabel(BundleCategory c) { return info(c).label; }
const char* categoryDescription(BundleCategory c) { return info(c).description; }

SupportBundleComposer::SupportBundleComposer() {
    for (std::size_t i = 0; i < static_cast<std::size_t>(BundleCategory::Count); ++i) selected_[i] = kCats[i].defaultOn;
}

Status SupportBundleComposer::addItem(BundleCategory category, const std::string& name, const std::string& content) {
    if (static_cast<std::size_t>(category) >= static_cast<std::size_t>(BundleCategory::Count)) return bad("unknown category");
    if (isForbidden(category)) return denied("this category is never included");
    if (items_.size() >= kBundleMaxItems) return makeError(ErrorCategory::ResourceExhausted, "support bundle: too many items");
    if (content.size() > kBundleMaxItemBytes) return makeError(ErrorCategory::ResourceExhausted, "support bundle: item too large");
    if (!security::canonicalMemberPath(name).ok() || name == "manifest.txt") return bad("unsafe item name");
    if (forbiddenExtension(name)) return denied("file type is never included");
    const char* why = nullptr;
    if (looksBinaryOrKey(content, why)) return denied(why);
    for (const auto& it : items_) if (it.name == name) return bad("duplicate item name");
    items_.push_back({category, name, content});
    return okStatus();
}

void SupportBundleComposer::setSelected(BundleCategory category, bool on) {
    if (static_cast<std::size_t>(category) < static_cast<std::size_t>(BundleCategory::Count) && !isForbidden(category))
        selected_[static_cast<std::size_t>(category)] = on;
}

std::vector<CategoryPreview> SupportBundleComposer::preview() const {
    std::vector<CategoryPreview> out;
    for (std::size_t c = 0; c < static_cast<std::size_t>(BundleCategory::Count); ++c) {
        const auto cat = static_cast<BundleCategory>(c);
        if (isForbidden(cat)) continue;
        CategoryPreview p;
        p.category = cat; p.label = categoryLabel(cat); p.description = categoryDescription(cat); p.selected = selected_[c];
        for (const auto& it : items_) {
            if (it.category != cat) continue;
            const auto r = redactor_.apply(it.content);
            ++p.itemCount; p.bytes += r.text.size(); p.redactions += r.replacements; p.itemNames.push_back(it.name);
            if (p.sampleLines.empty()) p.sampleLines = firstLines(r.text, 3);
        }
        if (p.itemCount) out.push_back(std::move(p));
    }
    return out;
}

Result<ComposedBundle> SupportBundleComposer::compose() const {
    ComposedBundle b;
    std::string manifest = "StageSignal support bundle\nThis archive contains only the categories listed below.\n"
                           "Never included: raw audio, camera frames, stream keys or other key material, show media files, crash dumps.\n\n";
    std::uint64_t total = 0;
    for (std::size_t c = 0; c < static_cast<std::size_t>(BundleCategory::Count); ++c) {
        const auto cat = static_cast<BundleCategory>(c);
        if (isForbidden(cat) || !selected_[c]) continue;
        std::size_t n = 0; std::uint64_t bytes = 0; int reds = 0;
        for (const auto& it : items_) {
            if (it.category != cat) continue;
            auto r = redactor_.apply(it.content);
            // fail closed: nothing secret-like may survive redaction
            if (redactor_.apply(r.text).replacements != 0) return denied("redaction did not converge; bundle refused");
            total += r.text.size();
            if (total > kBundleMaxTotalBytes) return makeError(ErrorCategory::ResourceExhausted, "support bundle: total size over limit");
            ++n; bytes += r.text.size(); reds += r.replacements;
            b.totalRedactions += r.replacements;
            b.files.push_back({it.name, std::move(r.text)});
        }
        if (n) manifest += std::string("- ") + categoryLabel(cat) + ": " + std::to_string(n) + " file(s), " + std::to_string(bytes) + " bytes, " +
                           std::to_string(reds) + " redaction(s)\n";
    }
    if (b.files.empty()) return bad("nothing selected");
    manifest += "\nTotal redactions applied: " + std::to_string(b.totalRedactions) + "\n";
    total += manifest.size();
    b.files.insert(b.files.begin(), BundleFile{"manifest.txt", manifest});
    b.totalBytes = total;
    return b;
}

std::vector<std::string> scanBundleForSecrets(const ComposedBundle& b, const security::Redactor& r) {
    std::vector<std::string> hits;
    for (const auto& f : b.files) if (r.apply(f.content).replacements != 0) hits.push_back(f.name);
    return hits;
}

}  // namespace ss::diag
