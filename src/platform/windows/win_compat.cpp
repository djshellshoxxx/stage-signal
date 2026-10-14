// XP-load shim. The mingw-w64 winpthread runtime (used by std::thread/std::mutex) imports kernel32!GetTickCount64
// (Vista+), which would stop the whole binary from loading on XP. Defining the import-table slot here makes the linker
// resolve winpthread's reference to this function instead of pulling the kernel32 import; on Vista+ it forwards to the real API.
#include "win_internal.h"

namespace {
typedef ULONGLONG (WINAPI *Tick64Fn)(void);

ULONGLONG WINAPI tick64Shim(void) {
  static Tick64Fn real = ss::win::detail::procAddr<Tick64Fn>(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64");
  if (real) return real();
  static CRITICAL_SECTION cs; static LONG init = 0; static DWORD last = 0; static ULONGLONG hi = 0;
  if (InterlockedCompareExchange(&init, 1, 0) == 0) { InitializeCriticalSection(&cs); InterlockedExchange(&init, 2); }
  while (init != 2) Sleep(0);
  EnterCriticalSection(&cs);
  DWORD now = GetTickCount();
  if (now < last) hi += (1ULL << 32);        // 32-bit counter wrapped (49.7 days)
  last = now;
  ULONGLONG r = hi + now;
  LeaveCriticalSection(&cs);
  return r;
}
}

#ifdef __i386__
extern "C" { void* ss_imp_GetTickCount64 __asm__("__imp__GetTickCount64@0") = reinterpret_cast<void*>(&tick64Shim); }
#else
extern "C" { void* ss_imp_GetTickCount64 __asm__("__imp_GetTickCount64") = reinterpret_cast<void*>(&tick64Shim); }
#endif
