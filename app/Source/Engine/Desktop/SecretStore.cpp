// OS keychain secret store (app-adapters 10, APP-AD-11). Windows: Credential Manager. Linux: libsecret via dlopen
// (no hard dependency). macOS: Keychain generic passwords through Security.framework (linked by app/CMakeLists.txt); compiled
// by CI on both macOS architectures but NOT exercised against a real keychain yet. There is deliberately no file or in-memory
// fallback: without a keychain, put() is refused.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <juce_core/juce_core.h>
#include "Engine/Desktop/DesktopCommon.h"
#include "Engine/Desktop/DesktopSystem.h"

#if JUCE_WINDOWS
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <wincred.h>
 #pragma comment(lib, "advapi32.lib")
#elif JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
 #include <Security/Security.h>
#elif JUCE_LINUX
 #include <dlfcn.h>
#endif

namespace ss::app {

namespace {
ss::Error unavailable(const std::string& why) { return desktop::err(ss::ErrorCategory::UnsupportedCapability, why); }
}

struct OsSecretStore::Impl {
    mutable std::mutex m;
    bool started = false, usable = false;
    std::string note;
    const char* backend = "none";
    std::set<std::string> refs;                     // references only (never values), persisted for list()

    juce::File indexFile() const {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("StageSignal").getChildFile("secret-refs.txt");
    }
    void loadIndex() {
        refs.clear();
        const auto f = indexFile();
        if (!f.existsAsFile()) return;
        juce::StringArray lines; f.readLines(lines);
        for (const auto& l : lines) { const std::string r = l.toStdString(); if (ss::security::isValidSecretRef(r)) refs.insert(r); }
    }
    void saveIndex() const {
        const auto f = indexFile();
        f.getParentDirectory().createDirectory();
        juce::String s; for (const auto& r : refs) s += juce::String(r) + "\n";
        f.replaceWithText(s);
    }

#if JUCE_WINDOWS
    static std::wstring target(const std::string& ref) { return juce::String("StageSignal/" + ref).toWideCharPointer(); }
    bool probe() { backend = "credman"; return true; }
    ss::Status put(const std::string& ref, const std::string& v) {
        std::wstring t = target(ref);
        CREDENTIALW c{};
        c.Type = CRED_TYPE_GENERIC; c.TargetName = const_cast<LPWSTR>(t.c_str());
        c.CredentialBlobSize = static_cast<DWORD>(v.size()); c.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(v.data()));
        c.Persist = CRED_PERSIST_LOCAL_MACHINE;
        return CredWriteW(&c, 0) ? ss::okStatus() : ss::Status(desktop::err(ss::ErrorCategory::PermissionDenied, "credential store write failed"));
    }
    ss::Result<std::string> get(const std::string& ref) {
        PCREDENTIALW c = nullptr;
        if (!CredReadW(target(ref).c_str(), CRED_TYPE_GENERIC, 0, &c)) return desktop::err(ss::ErrorCategory::InvalidArgument, "secret not found");
        std::string v(reinterpret_cast<const char*>(c->CredentialBlob), c->CredentialBlobSize);
        SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
        CredFree(c);
        return v;
    }
    ss::Status del(const std::string& ref) { CredDeleteW(target(ref).c_str(), CRED_TYPE_GENERIC, 0); return ss::okStatus(); }
#elif JUCE_LINUX
    // Minimal mirror of libsecret's SecretSchema layout (stable ABI).
    struct SchemaAttr { const char* name; int type; };
    struct Schema { const char* name; int flags; SchemaAttr attrs[32]; int reserved; void *r1, *r2, *r3, *r4, *r5, *r6, *r7; };
    struct GErr { unsigned domain; int code; char* message; };
    using StoreFn = int (*)(const Schema*, const char*, const char*, const char*, void*, GErr**, ...);
    using LookupFn = char* (*)(const Schema*, void*, GErr**, ...);
    using ClearFn = int (*)(const Schema*, void*, GErr**, ...);
    using FreeFn = void (*)(char*);
    using ErrFreeFn = void (*)(GErr*);
    void *lib = nullptr, *glib = nullptr;
    StoreFn store = nullptr; LookupFn lookup = nullptr; ClearFn clear = nullptr; FreeFn pwFree = nullptr; ErrFreeFn errFree = nullptr;
    Schema schema{};
    bool probe() {
        lib = dlopen("libsecret-1.so.0", RTLD_LAZY | RTLD_LOCAL);
        glib = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!lib || !glib) { note = "libsecret is not installed"; return false; }
        store = reinterpret_cast<StoreFn>(dlsym(lib, "secret_password_store_sync"));
        lookup = reinterpret_cast<LookupFn>(dlsym(lib, "secret_password_lookup_sync"));
        clear = reinterpret_cast<ClearFn>(dlsym(lib, "secret_password_clear_sync"));
        pwFree = reinterpret_cast<FreeFn>(dlsym(lib, "secret_password_free"));
        errFree = reinterpret_cast<ErrFreeFn>(dlsym(glib, "g_error_free"));
        if (!store || !lookup || !clear || !pwFree || !errFree) { note = "libsecret symbols missing"; return false; }
        std::memset(&schema, 0, sizeof(schema));
        schema.name = "org.stagesignal.Secret"; schema.flags = 0; schema.attrs[0] = {"ref", 0};
        // Service reachability: a lookup of an absent key returns null WITHOUT an error when the service is up.
        GErr* e = nullptr;
        char* r = lookup(&schema, nullptr, &e, "ref", "probe-absent", nullptr);
        if (r) pwFree(r);
        if (e) { errFree(e); note = "no Secret Service (keyring) is running"; return false; }
        backend = "libsecret";
        return true;
    }
    ss::Status put(const std::string& ref, const std::string& v) {
        GErr* e = nullptr;
        const int ok = store(&schema, "default", ref.c_str(), v.c_str(), nullptr, &e, "ref", ref.c_str(), nullptr);
        if (e) errFree(e);
        return ok ? ss::okStatus() : ss::Status(desktop::err(ss::ErrorCategory::PermissionDenied, "keyring write failed"));
    }
    ss::Result<std::string> get(const std::string& ref) {
        GErr* e = nullptr;
        char* r = lookup(&schema, nullptr, &e, "ref", ref.c_str(), nullptr);
        if (e) errFree(e);
        if (!r) return desktop::err(ss::ErrorCategory::InvalidArgument, "secret not found");
        std::string v(r); std::memset(r, 0, v.size()); pwFree(r);
        return v;
    }
    ss::Status del(const std::string& ref) { GErr* e = nullptr; clear(&schema, nullptr, &e, "ref", ref.c_str(), nullptr); if (e) errFree(e); return ss::okStatus(); }
#elif JUCE_MAC
    // Generic-password items, service "com.circuitdriftlabs.stagesignal", account = secret reference. Items are
    // WhenUnlockedThisDeviceOnly: not synced to iCloud and unreadable while the Mac is locked.
    struct CfString {
        CFStringRef s = nullptr;
        explicit CfString(const std::string& v)
            : s(CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(v.data()), static_cast<CFIndex>(v.size()),
                                        kCFStringEncodingUTF8, false)) {}
        ~CfString() { if (s) CFRelease(s); }
        CfString(const CfString&) = delete;
        CfString& operator=(const CfString&) = delete;
    };
    static CFMutableDictionaryRef newDict() {
        return CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    static CFMutableDictionaryRef itemQuery(const CfString& account) {
        CFMutableDictionaryRef q = newDict();
        CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(q, kSecAttrService, CFSTR("com.circuitdriftlabs.stagesignal"));
        CFDictionarySetValue(q, kSecAttrAccount, account.s);
        return q;
    }
    bool probe() {
        CfString account("probe-absent");
        CFMutableDictionaryRef q = itemQuery(account);
        CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef out = nullptr;
        const OSStatus st = SecItemCopyMatching(q, &out);
        if (out) CFRelease(out);
        CFRelease(q);
        if (st == errSecSuccess || st == errSecItemNotFound) { backend = "keychain"; return true; }
        note = "the macOS keychain is not available (status " + std::to_string(static_cast<int>(st)) + ")";
        return false;
    }
    ss::Status put(const std::string& ref, const std::string& v) {
        CfString account(ref);
        CFMutableDictionaryRef q = itemQuery(account);
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(v.data()), static_cast<CFIndex>(v.size()));
        CFMutableDictionaryRef update = newDict();
        CFDictionarySetValue(update, kSecValueData, data);
        OSStatus st = SecItemUpdate(q, update);
        if (st == errSecItemNotFound) {
            CFDictionarySetValue(q, kSecValueData, data);
            CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
            st = SecItemAdd(q, nullptr);
        }
        CFRelease(update); CFRelease(data); CFRelease(q);
        return st == errSecSuccess ? ss::okStatus() : ss::Status(desktop::err(ss::ErrorCategory::PermissionDenied, "keychain write failed"));
    }
    ss::Result<std::string> get(const std::string& ref) {
        CfString account(ref);
        CFMutableDictionaryRef q = itemQuery(account);
        CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef out = nullptr;
        const OSStatus st = SecItemCopyMatching(q, &out);
        CFRelease(q);
        if (st != errSecSuccess || !out) { if (out) CFRelease(out); return desktop::err(ss::ErrorCategory::InvalidArgument, "secret not found"); }
        const CFDataRef data = static_cast<CFDataRef>(out);
        std::string v(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<std::size_t>(CFDataGetLength(data)));
        CFRelease(out);
        return v;
    }
    ss::Status del(const std::string& ref) {
        CfString account(ref);
        CFMutableDictionaryRef q = itemQuery(account);
        SecItemDelete(q);                         // absent is fine: removal is idempotent
        CFRelease(q);
        return ss::okStatus();
    }
#else
    bool probe() { note = "no OS keychain adapter for this platform"; return false; }
    ss::Status put(const std::string&, const std::string&) { return unavailable("no keychain"); }
    ss::Result<std::string> get(const std::string&) { return unavailable("no keychain"); }
    ss::Status del(const std::string&) { return ss::okStatus(); }
#endif
};

OsSecretStore::OsSecretStore() : impl_(std::make_unique<Impl>()) {}
OsSecretStore::~OsSecretStore() = default;

ss::Status OsSecretStore::start() {
    std::lock_guard<std::mutex> g(impl_->m);
    if (impl_->started) return ss::okStatus();
    impl_->started = true;
    impl_->usable = impl_->probe();
    if (impl_->usable) impl_->loadIndex();
    return ss::okStatus();          // "no keychain" is a reported state (health), not a startup failure
}

void OsSecretStore::stop() { std::lock_guard<std::mutex> g(impl_->m); impl_->started = false; }

AdapterHealth OsSecretStore::health() const {
    std::lock_guard<std::mutex> g(impl_->m);
    if (!impl_->started) return {AdapterState::Stopped, {}};
    if (!impl_->usable) return {AdapterState::Unavailable, desktop::clean(impl_->note + "; secrets cannot be saved")};
    return {AdapterState::Running, {}};
}

const char* OsSecretStore::backendName() const { std::lock_guard<std::mutex> g(impl_->m); return impl_->backend; }

ss::Status OsSecretStore::put(const ss::security::SecretRef& r, const std::string& v) {
    if (!ss::security::isValidSecretRef(r)) return desktop::err(ss::ErrorCategory::InvalidArgument, "invalid secret reference");
    if (v.empty() || v.size() > ss::security::kMaxSecretBytes) return desktop::err(ss::ErrorCategory::InvalidArgument, "invalid secret size");
    std::lock_guard<std::mutex> g(impl_->m);
    if (!impl_->started || !impl_->usable) return unavailable("no OS keychain available; secret not stored");
    auto st = impl_->put(r, v);
    if (st.ok()) { impl_->refs.insert(r); impl_->saveIndex(); }
    return st;
}

ss::Result<std::string> OsSecretStore::get(const ss::security::SecretRef& r) const {
    if (!ss::security::isValidSecretRef(r)) return desktop::err(ss::ErrorCategory::InvalidArgument, "invalid secret reference");
    std::lock_guard<std::mutex> g(impl_->m);
    if (!impl_->started || !impl_->usable) return unavailable("no OS keychain available");
    return impl_->get(r);
}

ss::Status OsSecretStore::remove(const ss::security::SecretRef& r) {
    if (!ss::security::isValidSecretRef(r)) return desktop::err(ss::ErrorCategory::InvalidArgument, "invalid secret reference");
    std::lock_guard<std::mutex> g(impl_->m);
    if (!impl_->started || !impl_->usable) return unavailable("no OS keychain available");
    impl_->del(r);
    impl_->refs.erase(r); impl_->saveIndex();
    return ss::okStatus();
}

bool OsSecretStore::exists(const ss::security::SecretRef& r) const {
    std::lock_guard<std::mutex> g(impl_->m);
    return impl_->usable && impl_->refs.count(r) > 0;
}

std::vector<ss::security::SecretRef> OsSecretStore::list() const {
    std::lock_guard<std::mutex> g(impl_->m);
    return std::vector<ss::security::SecretRef>(impl_->refs.begin(), impl_->refs.end());
}

}  // namespace ss::app
