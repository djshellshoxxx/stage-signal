// System info and process/system counters for the perf agent. Newer APIs are resolved at runtime.
#include "win_internal.h"
#include <psapi.h>
#include <cstring>
#include <cstdio>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

namespace ss { namespace win {
using namespace detail;
namespace {

typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW*);
typedef BOOL (WINAPI *IsWow64ProcessFn)(HANDLE, PBOOL);
typedef DWORD (WINAPI *PowerGetActiveSchemeFn)(HKEY, GUID**);
typedef DWORD (WINAPI *PowerReadFriendlyNameFn)(HKEY, const GUID*, const GUID*, const GUID*, PUCHAR, LPDWORD);
typedef LONG (WINAPI *NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);
typedef BOOL (WINAPI *GetSystemTimesFn)(FILETIME*, FILETIME*, FILETIME*);
typedef BOOL (WINAPI *GetProcessMemoryInfoFn)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);

uint64_t ft(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

std::string regString(HKEY root, const wchar_t* key, const wchar_t* name) {
  HKEY h; if (RegOpenKeyExW(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return std::string();
  wchar_t buf[256]; DWORD sz = sizeof buf - sizeof(wchar_t), type = 0; std::string out;
  if (RegQueryValueExW(h, name, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && type == REG_SZ) { buf[sz / sizeof(wchar_t)] = 0; out = narrow(buf, wcslen(buf)); }
  RegCloseKey(h); return out;
}

} // namespace

std::string windowsVersionString() {
  OSVERSIONINFOW v; ZeroMemory(&v, sizeof v); v.dwOSVersionInfoSize = sizeof v;
  bool got = false;
  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  RtlGetVersionFn rtl = procAddr<RtlGetVersionFn>(nt, "RtlGetVersion");
  if (rtl && rtl(&v) == 0) got = true;       // not subject to manifest-based version lies
  if (!got) { if (!GetVersionExW(&v)) return "Windows (unknown version)"; }
  char b[160];
  std::snprintf(b, sizeof b, "Windows %lu.%lu.%lu", (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber);
  std::string s = b;
  if (v.szCSDVersion[0]) s += " " + narrow(v.szCSDVersion, wcsnlen(v.szCSDVersion, 128));
  SYSTEM_INFO si; GetNativeSystemInfo(&si);
  const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64" : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "x86" : si.wProcessorArchitecture == 12 ? "arm64" : "other";
  s += std::string(" (") + arch;
  IsWow64ProcessFn w64 = procAddr<IsWow64ProcessFn>(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
  BOOL wow = FALSE; if (w64 && w64(GetCurrentProcess(), &wow) && wow) s += ", 32-bit process under WOW64";
  s += ")";
  return s;
}

std::string powerPlanName() {
  HMODULE m = LoadLibraryW(L"powrprof.dll");
  if (!m) return "unavailable";
  PowerGetActiveSchemeFn get = procAddr<PowerGetActiveSchemeFn>(m, "PowerGetActiveScheme");
  PowerReadFriendlyNameFn rd = procAddr<PowerReadFriendlyNameFn>(m, "PowerReadFriendlyName");
  std::string out = "unavailable";
  GUID* g = nullptr;
  if (get && get(nullptr, &g) == ERROR_SUCCESS && g) {
    static const GUID balanced = {0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
    static const GUID high = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
    static const GUID saver = {0xa1841308, 0x3541, 0x4fab, {0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};
    bool named = false;
    if (rd) {
      WCHAR buf[256]; DWORD sz = sizeof buf;
      if (rd(nullptr, g, nullptr, nullptr, (PUCHAR)buf, &sz) == ERROR_SUCCESS && buf[0]) { out = narrow(buf, wcsnlen(buf, 255)); named = true; }
    }
    if (!named) out = memcmp(g, &balanced, sizeof(GUID)) == 0 ? "Balanced" : memcmp(g, &high, sizeof(GUID)) == 0 ? "High performance" : memcmp(g, &saver, sizeof(GUID)) == 0 ? "Power saver" : "custom plan";
    LocalFree(g);
  }
  FreeLibrary(m);
  return out;
}

std::string cpuModelString() {
  std::string name = trimmed(regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString"));
  if (name.empty()) {
#if defined(__i386__) || defined(__x86_64__)
    unsigned a, b, c, d;
    if (__get_cpuid(0x80000000u, &a, &b, &c, &d) && a >= 0x80000004u) {
      char br[49] = {0};
      for (unsigned i = 0; i < 3; ++i) { __cpuid(0x80000002u + i, a, b, c, d); memcpy(br + 16 * i, &a, 4); memcpy(br + 16 * i + 4, &b, 4); memcpy(br + 16 * i + 8, &c, 4); memcpy(br + 16 * i + 12, &d, 4); }
      name = trimmed(br);
    }
#endif
  }
  if (name.empty()) name = "unknown CPU";
  HKEY h; DWORD mhz = 0, sz = sizeof mhz, type = 0;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &h) == ERROR_SUCCESS) {
    if (RegQueryValueExW(h, L"~MHz", nullptr, &type, (BYTE*)&mhz, &sz) == ERROR_SUCCESS && type == REG_DWORD && mhz) name += " (~" + std::to_string(mhz) + " MHz)";
    RegCloseKey(h);
  }
  return name;
}

bool readSysCounters(SysCounters& o) {
  o = SysCounters();
  o.wallNs = (uint64_t)qpcNowNs();
  SYSTEM_INFO si; GetSystemInfo(&si); o.logicalCores = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
  FILETIME cr, ex, k, u;
  bool p = GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u) != 0;
  if (p) { o.procKernel100ns = ft(k); o.procUser100ns = ft(u); }
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  GetSystemTimesFn gst = procAddr<GetSystemTimesFn>(k32, "GetSystemTimes");   // XP SP1+
  FILETIME id, sk, su; bool s = gst && gst(&id, &sk, &su);
  if (s) { o.sysIdle100ns = ft(id); o.sysKernel100ns = ft(sk); o.sysUser100ns = ft(su); }
  o.valid = p && s;

  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  NtQuerySystemInformationFn nq = procAddr<NtQuerySystemInformationFn>(nt, "NtQuerySystemInformation");
  if (nq) {
    const ULONG per = 48;                   // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION: 5 x int64 + ULONG, 8-byte padded
    std::vector<uint8_t> buf((size_t)o.logicalCores * per);
    ULONG ret = 0;
    if (nq(8 /*SystemProcessorPerformanceInformation*/, buf.data(), (ULONG)buf.size(), &ret) == 0 && ret >= buf.size()) {
      o.coreIdle100ns.resize(o.logicalCores); o.coreTotal100ns.resize(o.logicalCores);
      for (uint32_t i = 0; i < o.logicalCores; ++i) {
        uint64_t v[3]; memcpy(v, buf.data() + (size_t)i * per, sizeof v);   // idle, kernel (incl. idle), user
        o.coreIdle100ns[i] = v[0]; o.coreTotal100ns[i] = v[1] + v[2];
      }
      o.perCoreAvailable = true;
    }
  }

  static GetProcessMemoryInfoFn gpmi = [] {
    HMODULE m = LoadLibraryW(L"psapi.dll"); return procAddr<GetProcessMemoryInfoFn>(m, "GetProcessMemoryInfo"); }();
  if (gpmi) {
    PROCESS_MEMORY_COUNTERS_EX pm; ZeroMemory(&pm, sizeof pm); pm.cb = sizeof pm;
    if (gpmi(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm)) {
      o.workingSetBytes = pm.WorkingSetSize; o.peakWorkingSetBytes = pm.PeakWorkingSetSize; o.privateBytes = pm.PrivateUsage; o.memoryValid = true;
    }
  }
  MEMORYSTATUSEX ms; ZeroMemory(&ms, sizeof ms); ms.dwLength = sizeof ms;
  if (GlobalMemoryStatusEx(&ms)) { o.physTotalBytes = ms.ullTotalPhys; o.physAvailBytes = ms.ullAvailPhys; }
  return o.valid;
}

}} // namespace
