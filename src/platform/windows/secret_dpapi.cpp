// DPAPI per-user secret store (MOD-15, REQ-B03). Nothing here logs; Status details never contain secret or ref content.
#include "win_internal.h"
#include <wincrypt.h>
#include <sddl.h>
#include <cstring>

namespace ss { namespace win {
using namespace detail;
namespace {

std::wstring secretsDir() {
  wchar_t buf[MAX_PATH * 2]; DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
  if (!n || n >= MAX_PATH * 2) { n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH * 2); if (!n || n >= MAX_PATH * 2) return std::wstring(); }
  return std::wstring(buf, n) + L"\\StageSignal\\secrets";
}
std::string userSidString() {
  HANDLE tok = nullptr; std::string out;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
  DWORD n = 0; GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
  if (n) {
    std::vector<uint8_t> b(n);
    if (GetTokenInformation(tok, TokenUser, b.data(), n, &n)) {
      LPWSTR s = nullptr;
      if (ConvertSidToStringSidW(((TOKEN_USER*)b.data())->User.Sid, &s) && s) { out = narrow(s, wcslen(s)); LocalFree(s); }
    }
  }
  CloseHandle(tok); return out;
}
void ensureDir(const std::wstring& dir) {
  // create each component; the leaf gets a protected DACL (SYSTEM + current user only) when possible
  std::wstring cur;
  for (size_t i = 0; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == L'\\') {
      if (!cur.empty() && cur.size() > 2) CreateDirectoryW(cur.c_str(), nullptr);
    }
    if (i < dir.size()) cur += dir[i];
  }
  std::string sid = userSidString();
  if (sid.empty()) return;
  std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + widen(sid) + L")";
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr) && sd) {
    SetFileSecurityW(dir.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd);   // best effort
    LocalFree(sd);
  }
}
std::wstring fileFor(const std::string& ref) {
  uint64_t a = fnv1a64(ref.data(), ref.size());
  uint64_t b = fnv1a64(ref.data(), ref.size(), 0x9E3779B97F4A7C15ULL);
  return secretsDir() + L"\\" + widen(hex(a) + hex(b)) + L".bin";
}

class Dpapi : public ISecretStore {
 public:
  Status put(const std::string& ref, const std::string& secret) override {
    if (ref.empty()) return Status::Fail(Err::InvalidArgument, "secret reference required");
    std::wstring dir = secretsDir(); if (dir.empty()) return Status::Fail(Err::InternalFault, "no per-user profile directory");
    ensureDir(dir);
    DATA_BLOB in, ent, out; ZeroMemory(&out, sizeof out);
    in.cbData = (DWORD)secret.size(); in.pbData = (BYTE*)(secret.empty() ? (const char*)"" : secret.data());
    ent.cbData = (DWORD)ref.size(); ent.pbData = (BYTE*)ref.data();       // bind blob to its reference
    if (!CryptProtectData(&in, L"StageSignal secret", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
      return Status::Fail(Err::InternalFault, "DPAPI protect failed (" + std::to_string(GetLastError()) + ")");
    std::wstring path = fileFor(ref), tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (h == INVALID_HANDLE_VALUE) { LocalFree(out.pbData); return Status::Fail(Err::InternalFault, "cannot write secret file (" + std::to_string(GetLastError()) + ")"); }
    DWORD wr = 0; bool ok = WriteFile(h, out.pbData, out.cbData, &wr, nullptr) && wr == out.cbData && FlushFileBuffers(h);
    CloseHandle(h); LocalFree(out.pbData);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      DeleteFileW(tmp.c_str()); return Status::Fail(Err::DiskFull, "secret file write failed");
    }
    return Status::Ok();
  }
  Status get(const std::string& ref, std::string& out) override {
    out.clear();
    if (ref.empty()) return Status::Fail(Err::InvalidArgument, "secret reference required");
    std::wstring path = fileFor(ref);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Status::Fail(Err::InvalidArgument, "secret not found");
    DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE || sz > (1u << 20)) { CloseHandle(h); return Status::Fail(Err::InternalFault, "secret file invalid"); }
    std::vector<uint8_t> blob(sz); DWORD rd = 0;
    bool ok = ReadFile(h, blob.data(), sz, &rd, nullptr) && rd == sz; CloseHandle(h);
    if (!ok) return Status::Fail(Err::InternalFault, "secret file unreadable");
    DATA_BLOB in, ent, res; ZeroMemory(&res, sizeof res);
    in.cbData = sz; in.pbData = blob.data(); ent.cbData = (DWORD)ref.size(); ent.pbData = (BYTE*)ref.data();
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &res))
      return Status::Fail(Err::PermissionDenied, "secret cannot be decrypted for this user");
    out.assign((const char*)res.pbData, res.cbData);
    SecureZeroMemory(res.pbData, res.cbData); LocalFree(res.pbData);
    return Status::Ok();
  }
  Status remove(const std::string& ref) override {
    if (ref.empty()) return Status::Fail(Err::InvalidArgument, "secret reference required");
    std::wstring path = fileFor(ref);
    if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return Status::Fail(Err::InternalFault, "cannot delete secret file");
    return Status::Ok();       // idempotent
  }
};

} // namespace
std::unique_ptr<ISecretStore> makeSecretStore() { return std::unique_ptr<ISecretStore>(new Dpapi()); }
}} // namespace
