// StageSignal security: secret storage interface (spec section 16). Secrets are machine-local; show files and
// settings carry only SecretRef strings. The OS-backed implementation (Credential Manager, Keychain, libsecret)
// is app layer; InMemorySecretStore is the fake used by tests and by dry-run configurations.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/security/redact.hpp"

namespace ss::security {

using SecretRef = std::string;   // "secret:<label>", label = [a-z0-9_-]{1,64}

constexpr std::size_t kMaxSecretBytes = 4096;

bool isValidSecretRef(const std::string& ref);
SecretRef makeSecretRef(const std::string& label);   // lower-cases, maps other characters to '-', truncates

class ISecretStore {
public:
    virtual ~ISecretStore() = default;
    virtual Status put(const SecretRef& ref, const std::string& value) = 0;       // create or replace
    virtual Result<std::string> get(const SecretRef& ref) const = 0;
    virtual Status remove(const SecretRef& ref) = 0;                               // idempotent
    virtual bool exists(const SecretRef& ref) const = 0;
    virtual std::vector<SecretRef> list() const = 0;                               // references only
};

class InMemorySecretStore final : public ISecretStore {
public:
    Status put(const SecretRef& ref, const std::string& value) override;
    Result<std::string> get(const SecretRef& ref) const override;
    Status remove(const SecretRef& ref) override;
    bool exists(const SecretRef& ref) const override;
    std::vector<SecretRef> list() const override;
private:
    std::map<SecretRef, std::string> data_;
};

// A Redactor that also scrubs every value currently held by the store (belt and braces for logs and bundles).
Redactor makeRedactorFor(const ISecretStore& store);

}  // namespace ss::security
