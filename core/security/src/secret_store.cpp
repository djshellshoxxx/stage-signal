#include "ss/security/secret_store.hpp"

#include <cctype>

namespace ss::security {

bool isValidSecretRef(const std::string& ref) {
    static const std::string prefix = "secret:";
    if (ref.compare(0, prefix.size(), prefix) != 0) return false;
    const std::size_t n = ref.size() - prefix.size();
    if (n < 1 || n > 64) return false;
    for (std::size_t i = prefix.size(); i < ref.size(); ++i) {
        const char c = ref[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

SecretRef makeSecretRef(const std::string& label) {
    std::string out;
    for (char c : label) {
        if (out.size() >= 64) break;
        const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out += ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '_' || l == '-') ? l : '-';
    }
    if (out.empty()) out = "unnamed";
    return "secret:" + out;
}

Status InMemorySecretStore::put(const SecretRef& ref, const std::string& value) {
    if (!isValidSecretRef(ref)) return makeError(ErrorCategory::InvalidArgument, "invalid secret reference");
    if (value.empty() || value.size() > kMaxSecretBytes) return makeError(ErrorCategory::InvalidArgument, "secret value size out of range");
    data_[ref] = value;
    return okStatus();
}

Result<std::string> InMemorySecretStore::get(const SecretRef& ref) const {
    auto it = data_.find(ref);
    if (it == data_.end()) return makeError(ErrorCategory::InvalidArgument, "unknown secret reference");
    return it->second;
}

Status InMemorySecretStore::remove(const SecretRef& ref) {
    data_.erase(ref);
    return okStatus();
}

bool InMemorySecretStore::exists(const SecretRef& ref) const { return data_.count(ref) != 0; }

std::vector<SecretRef> InMemorySecretStore::list() const {
    std::vector<SecretRef> v;
    for (const auto& kv : data_) v.push_back(kv.first);
    return v;
}

Redactor makeRedactorFor(const ISecretStore& store) {
    Redactor r;
    for (const auto& ref : store.list()) {
        auto v = store.get(ref);
        if (v) r.addLiteral(v.value());
    }
    return r;
}

}  // namespace ss::security
