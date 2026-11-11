#include "ui/output_windows.h"
#include "ui/identify.h"
#include <vector>

namespace ss { namespace ui {
namespace {
struct OutData { std::string id; app::FramePtr last; bool diag = false; };

LRESULT CALLBACK OutProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  OutData* d = (OutData*)GetWindowLongPtrW(h, GWLP_USERDATA);
  switch (m) {
    case WM_NCCREATE: SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams); break;
    case WM_CREATE: SetTimer(h, 1, 40, nullptr); return 0;
    case WM_TIMER: if (d) { app::FramePtr f; if (A.ctl && A.ctl->outputFrame(d->id, f) && frameDiffers(f, d->last)) { d->last = f; InvalidateRect(h, nullptr, FALSE); } } return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
      paintFrame(dc, rc, d ? d->last.get() : nullptr);
      if (d && d->diag) {                       // reference border only when diagnostics were explicitly enabled for this output
        HPEN p1 = CreatePen(PS_SOLID, 3, RGB(255, 255, 255)); HGDIOBJ op = SelectObject(dc, p1), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(p1);
      }
      EndPaint(h, &ps); return 0;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR: SetCursor(nullptr); return TRUE;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: if (A.mainWnd) PostMessageW(A.mainWnd, WM_SS_KEY, w, l); return 0;
    case WM_CLOSE: return 0;                    // routes are managed by AppController, not by closing windows
    case WM_NCDESTROY: KillTimer(h, 1); delete d; SetWindowLongPtrW(h, GWLP_USERDATA, 0); break;
  }
  return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK IdentProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: SetTimer(h, 1, 5000, nullptr); return 0;
    case WM_TIMER: DestroyWindow(h); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_PAINT: {
      PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
      HBRUSH b = CreateSolidBrush(RGB(0, 0, 0)); FillRect(dc, &rc, b); DeleteObject(b);
      HPEN pen = CreatePen(PS_SOLID, 4, RGB(255, 255, 255)); HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH)); Rectangle(dc, 2, 2, rc.right - 2, rc.bottom - 2); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
      wchar_t t[256]; GetWindowTextW(h, t, 255); SelectObject(dc, g.big); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
      DrawTextW(dc, t, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); EndPaint(h, &ps); return 0;
    }
  }
  return DefWindowProcW(h, m, w, l);
}
} // namespace

void OutputWindows::registerClass(HINSTANCE hi) {
  WNDCLASSW wc; ZeroMemory(&wc, sizeof wc); wc.hInstance = hi; wc.hCursor = nullptr; wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  wc.lpfnWndProc = OutProc; wc.lpszClassName = L"SSAudienceOutput"; RegisterClassW(&wc);
  wc.lpfnWndProc = IdentProc; wc.lpszClassName = L"SSIdentify"; RegisterClassW(&wc);
}

void OutputWindows::sync() {
  if (!A.ctl) return;
  auto routes = A.ctl->outputRoutes(); std::vector<std::string> keep;
  for (auto& r : routes) {
    bool want = r.state == app::RouteState::Active && r.role != show::OutputRole::Controls && r.w > 0 && r.h > 0;
    if (!want) continue;
    keep.push_back(r.logicalId);
    auto it = wins_.find(r.logicalId);
    if (it == wins_.end()) {
      OutData* d = new OutData(); d->id = r.logicalId; d->diag = r.diagnostics;
      HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"SSAudienceOutput", W("StageSignal output " + r.logicalId).c_str(), WS_POPUP,
                               r.x, r.y, r.w, r.h, nullptr, nullptr, GetModuleHandleW(nullptr), d);
      if (!h) { delete d; continue; }
      ShowWindow(h, SW_SHOWNOACTIVATE); SetWindowPos(h, HWND_TOPMOST, r.x, r.y, r.w, r.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
      W w; w.h = h; w.generation = r.generation; wins_[r.logicalId] = w;
    } else {
      OutData* d = (OutData*)GetWindowLongPtrW(it->second.h, GWLP_USERDATA);
      if (d && d->diag != r.diagnostics) { d->diag = r.diagnostics; InvalidateRect(it->second.h, nullptr, FALSE); }
      if (it->second.generation != r.generation) { it->second.generation = r.generation; SetWindowPos(it->second.h, HWND_TOPMOST, r.x, r.y, r.w, r.h, SWP_NOACTIVATE); }
    }
  }
  for (auto it = wins_.begin(); it != wins_.end();) {
    bool k = false; for (auto& s : keep) if (s == it->first) k = true;
    if (!k) { DestroyWindow(it->second.h); it = wins_.erase(it); } else ++it;
  }
}
void OutputWindows::closeAll() { for (auto& kv : wins_) DestroyWindow(kv.second.h); wins_.clear(); }

void identifyDisplays() {
  if (!A.ctl) return;
  for (auto& d : A.ctl->displays()) {
    int w = std::min(d.w, S(420)), h = S(110); int x = d.x + (d.w - w) / 2, y = d.y + (d.h - h) / 2;
    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"SSIdentify", W(fmt("Display %d: %s", d.index + 1, d.name.c_str())).c_str(), WS_POPUP, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hw) ShowWindow(hw, SW_SHOWNOACTIVATE);
  }
}
}} // namespace ss::ui
