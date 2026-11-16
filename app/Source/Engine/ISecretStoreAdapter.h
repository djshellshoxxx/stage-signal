// Contract: OS keychain adapter (app-adapters 10, APP-AD-11). Owner: engine-adapters-desktop.
// Extends the security core's ISecretStore. Secrets never enter show files, logs or bundles.
#pragma once
#include "Engine/AdapterCommon.h"
#include "ss/security/secret_store.hpp"

namespace ss::app {

class ISecretStoreAdapter : public IAdapter, public ss::security::ISecretStore {
public:
    virtual const char* backendName() const = 0;     // "keychain", "credman", "libsecret", "memory" ...
    virtual bool persistent() const = 0;             // false for the in-memory fake
};

class MemorySecretStoreAdapter final : public ISecretStoreAdapter {
public:
    ss::Status start() override { return ss::okStatus(); }
    void stop() override {}
    AdapterHealth health() const override { return {AdapterState::Running, {}}; }
    const char* backendName() const override { return "memory"; }
    bool persistent() const override { return false; }
    ss::Status put(const ss::security::SecretRef& r, const std::string& v) override { return s_.put(r, v); }
    ss::Result<std::string> get(const ss::security::SecretRef& r) const override { return s_.get(r); }
    ss::Status remove(const ss::security::SecretRef& r) override { return s_.remove(r); }
    bool exists(const ss::security::SecretRef& r) const override { return s_.exists(r); }
    std::vector<ss::security::SecretRef> list() const override { return s_.list(); }
private:
    ss::security::InMemorySecretStore s_;
};

}  // namespace ss::app
