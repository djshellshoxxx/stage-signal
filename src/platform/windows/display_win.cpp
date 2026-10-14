// Display enumeration with stable identity (REQ-D01..D06, IF-06). Index is never identity.
#include "win_internal.h"
#include "display_ident.h"
#include <atomic>
#include <algorithm>
#include <map>
#include <thread>
#include <condition_variable>

namespace ss { namespace win {
using namespace detail;

// ---- pure helpers (testable without a display) ---------------------------------------------
bool parseEdid(const uint8_t* e, size_t n, EdidInfo& out) {
  if (n < 128) return false;
  static const uint8_t hdr[8] = {0, 255, 255, 255, 255, 255, 255, 0};
  for (int i = 0; i < 8; ++i) if (e[i] != hdr[i]) return false;
  uint16_t m = (uint16_t)((e[8] << 8) | e[9]);
  char mfg[4] = {(char)('A' - 1 + ((m >> 10) & 31)), (char)('A' - 1 + ((m >> 5) & 31)), (char)('A' - 1 + (m & 31)), 0};
  out.mfg = mfg;
  out.product = (uint16_t)(e[10] | (e[11] << 8));
  out.serial = (uint32_t)e[12] | ((uint32_t)e[13] << 8) | ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
  out.name.clear(); out.serialText.clear();
  for (int off = 54; off <= 108; off += 18) {
    if (e[off] || e[off + 1] || e[off + 2]) continue;      // not a display descriptor
    uint8_t tag = e[off + 3];
    if (tag != 0xFC && tag != 0xFF) continue;
    std::string s;
    for (int i = 0; i < 13; ++i) { char c = (char)e[off + 5 + i]; if (c == '\n' || c == 0) break; s += c; }
    s = trimmed(s);
    if (tag == 0xFC) out.name = s; else out.serialText = s;
  }
  out.hash = fnv1a64(e, 128);
  return true;
}

bool splitInterfacePath(const std::string& path, std::string& model, std::string& instance) {
  // "\\?\DISPLAY#DEL4096#5&abc&0&UID4352#{guid}"  or  "DISPLAY\DEL4096\5&abc&0&UID4352"
  std::string p = path;
  if (p.compare(0, 4, "\\\\?\\") == 0) p = p.substr(4);
  for (auto& c : p) if (c == '#') c = '\\';
  size_t brace = p.find("\\{"); if (brace != std::string::npos) p = p.substr(0, brace);
  size_t a = p.find('\\'); if (a == std::string::npos) return false;
  size_t b = p.find('\\', a + 1); if (b == std::string::npos) return false;
  if (p.substr(0, a) != "DISPLAY") return false;
  model = p.substr(a + 1, b - a - 1); instance = p.substr(b + 1);
  return !model.empty();
}

namespace {

bool readEdidFromRegistry(const std::string& model, const std::string& instance, std::vector<uint8_t>& out) {
  std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" + widen(model) + L"\\" + widen(instance) + L"\\Device Parameters";
  HKEY h;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
  DWORD type = 0, sz = 0;
  bool ok = false;
  if (RegQueryValueExW(h, L"EDID", nullptr, &type, nullptr, &sz) == ERROR_SUCCESS && type == REG_BINARY && sz >= 128 && sz < 4096) {
    out.resize(sz);
    ok = RegQueryValueExW(h, L"EDID", nullptr, &type, out.data(), &sz) == ERROR_SUCCESS;
  }
  RegCloseKey(h);
  return ok;
}

typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);

int monitorDpi(HMONITOR mon, const wchar_t* dev) {
  static GetDpiForMonitorFn fn = [] {
    HMODULE m = LoadLibraryW(L"shcore.dll");
    return procAddr<GetDpiForMonitorFn>(m, "GetDpiForMonitor");
  }();
  UINT x = 0, y = 0;
  if (fn && SUCCEEDED(fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &x, &y)) && x) return (int)x;
  HDC dc = CreateDCW(L"DISPLAY", dev, nullptr, nullptr);
  int dpi = 96;
  if (dc) { int v = GetDeviceCaps(dc, LOGPIXELSX); if (v > 0) dpi = v; DeleteDC(dc); }
  return dpi;
}

struct Raw { HMONITOR mon; };
BOOL CALLBACK enumProc(HMONITOR m, HDC, LPRECT, LPARAM lp) { reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(m); return TRUE; }

std::vector<DisplayInfo> doEnumerate() {
  std::vector<HMONITOR> mons;
  EnumDisplayMonitors(nullptr, nullptr, enumProc, (LPARAM)&mons);
  std::vector<DisplayInfo> out;
  std::vector<std::string> modelOnly;      // for ambiguity when no EDID
  std::vector<bool> hadEdid;
  int idx = 0;
  for (HMONITOR m : mons) {
    MONITORINFOEXW mi; ZeroMemory(&mi, sizeof mi); mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(m, &mi)) continue;
    DisplayInfo d;
    d.index = idx++;
    d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    d.x = mi.rcMonitor.left; d.y = mi.rcMonitor.top;
    d.w = mi.rcMonitor.right - mi.rcMonitor.left; d.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    DEVMODEW dm; ZeroMemory(&dm, sizeof dm); dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
      if (dm.dmPelsWidth && dm.dmPelsHeight) {      // physical pixel geometry (unaffected by DPI virtualization)
        d.w = (int)dm.dmPelsWidth; d.h = (int)dm.dmPelsHeight;
        if (dm.dmFields & DM_POSITION) { d.x = dm.dmPosition.x; d.y = dm.dmPosition.y; }
      }
      if (dm.dmFields & DM_DISPLAYORIENTATION) d.rotation = (int)dm.dmDisplayOrientation * 90;
    }
    d.dpi = monitorDpi(m, mi.szDevice);
    // Monitor attached to this adapter output: take its device-interface path.
    std::string devPath, friendly, model, instance;
    for (DWORD i = 0; i < 8; ++i) {
      DISPLAY_DEVICEW dd; ZeroMemory(&dd, sizeof dd); dd.cb = sizeof dd;
      if (!EnumDisplayDevicesW(mi.szDevice, i, &dd, 0x1 /*EDD_GET_DEVICE_INTERFACE_NAME*/)) break;
      if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
      devPath = narrow(dd.DeviceID, wcsnlen(dd.DeviceID, 128));
      friendly = narrow(dd.DeviceString, wcsnlen(dd.DeviceString, 128));
      break;
    }
    EdidInfo ei; bool haveEdid = false;
    if (!devPath.empty() && splitInterfacePath(devPath, model, instance)) {
      std::vector<uint8_t> raw;
      if (readEdidFromRegistry(model, instance, raw)) haveEdid = parseEdid(raw.data(), raw.size(), ei);
    }
    if (haveEdid) {
      d.stableId = "edid:" + ei.mfg + hex(ei.product, 4) + ":" + hex(ei.hash, 16);
      d.name = !ei.name.empty() ? ei.name : (friendly.empty() ? ei.mfg + hex(ei.product, 4) : friendly);
      modelOnly.push_back("");
    } else if (!model.empty()) {
      d.stableId = "path:" + model + ":" + hex(fnv1a64(instance.data(), instance.size()), 16);
      d.name = friendly.empty() ? model : friendly;
      modelOnly.push_back(model);
    } else {
      // No monitor attached to this output (e.g. virtual/remote): identity from the adapter output name only.
      d.stableId = "out:" + hex(fnv1a64(mi.szDevice, wcslen(mi.szDevice) * sizeof(wchar_t)), 16);
      d.name = narrow(mi.szDevice, wcsnlen(mi.szDevice, 32));
      modelOnly.push_back("");
    }
    hadEdid.push_back(haveEdid);
    out.push_back(std::move(d));
  }
  // Ambiguity: identical stable ids (same EDID incl. serial), or path-based ids with same model code (identity not trustworthy).
  for (size_t i = 0; i < out.size(); ++i)
    for (size_t j = i + 1; j < out.size(); ++j) {
      bool same = out[i].stableId == out[j].stableId;
      bool sameModelNoEdid = !hadEdid[i] && !hadEdid[j] && !modelOnly[i].empty() && modelOnly[i] == modelOnly[j];
      if (same || sameModelNoEdid) out[i].ambiguous = out[j].ambiguous = true;
    }
  return out;
}

class DisplayProvider : public IDisplayProvider {
 public:
  ~DisplayProvider() override { stop(); }
  std::vector<DisplayInfo> enumerate() override { return doEnumerate(); }
  void setChangeCallback(std::function<void()> cb) override {
    { std::lock_guard<std::mutex> l(m_); cb_ = cb ? std::make_shared<std::function<void()>>(std::move(cb)) : nullptr; }
    if (cb_) startThread();
  }
 private:
  static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    DisplayProvider* self = reinterpret_cast<DisplayProvider*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
      case WM_NCCREATE: SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams); break;
      case WM_DISPLAYCHANGE:
        if (self) SetTimer(h, 1, 250, nullptr);       // debounce bursts into one callback
        break;
      case 0x0219 /*WM_DEVICECHANGE*/:
        if (self && wp == 0x0007 /*DBT_DEVNODES_CHANGED*/) SetTimer(h, 1, 250, nullptr);
        break;
      case WM_TIMER:
        if (self && wp == 1) {
          KillTimer(h, 1);
          std::shared_ptr<std::function<void()>> cb; { std::lock_guard<std::mutex> l(self->m_); cb = self->cb_; }
          if (cb) (*cb)();
        }
        return 0;
      default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
  }
  void startThread() {
    std::lock_guard<std::mutex> l(tm_);
    if (th_.joinable()) return;
    ready_ = false;
    th_ = std::thread([this] {
      WNDCLASSW wc; ZeroMemory(&wc, sizeof wc);
      wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"StageSignalDisplayWatcher";
      RegisterClassW(&wc);
      // Hidden top-level (never shown) window: message-only windows do not receive WM_DISPLAYCHANGE broadcasts.
      HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
      hwnd_ = h; tid_ = GetCurrentThreadId();
      { std::lock_guard<std::mutex> ll(rm_); ready_ = true; } rcv_.notify_all();
      MSG msg;
      while (h && GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
      if (h) DestroyWindow(h);
      UnregisterClassW(wc.lpszClassName, wc.hInstance);
    });
    std::unique_lock<std::mutex> ul(rm_);
    rcv_.wait_for(ul, std::chrono::seconds(2), [this] { return ready_; });
  }
  void stop() {
    std::lock_guard<std::mutex> l(tm_);
    if (th_.joinable()) { PostThreadMessageW(tid_, WM_QUIT, 0, 0); th_.join(); }
  }
  std::mutex m_, tm_, rm_; std::condition_variable rcv_; bool ready_ = false;
  std::shared_ptr<std::function<void()>> cb_;
  std::thread th_; HWND hwnd_ = nullptr; DWORD tid_ = 0;
};

} // namespace

std::unique_ptr<IDisplayProvider> makeDisplayProvider() { return std::unique_ptr<IDisplayProvider>(new DisplayProvider()); }

}} // namespace
