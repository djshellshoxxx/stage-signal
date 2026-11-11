#include "ui/ui_util.h"
#include "midi/mapping.h"
#include <shlobj.h>
#include <commdlg.h>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <map>
#include <mutex>

namespace ss { namespace ui {

Gfx g;
UiApp A;

// ---- strings ----------------------------------------------------------------------------------------------------------
std::wstring W(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  if (n <= 0) { std::wstring r; for (unsigned char c : s) r.push_back((wchar_t)c); return r; }
  std::wstring r((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &r[0], n);
  return r;
}
std::string U8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string r((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &r[0], n, nullptr, nullptr);
  return r;
}
std::string fmt(const char* f, ...) {
  char buf[1024]; va_list ap; va_start(ap, f); vsnprintf(buf, sizeof buf, f, ap); va_end(ap); buf[sizeof buf - 1] = 0; return buf;
}
std::string fmtDb(float db) { if (db <= -119.f) return "-inf dB"; return fmt("%.1f dB", (double)db); }
std::string fmtTime(double sec) {
  if (sec < 0) sec = 0; long t = (long)sec; return fmt("%02ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
}

// ---- gfx --------------------------------------------------------------------------------------------------------------------
static void setPalette() {
  HIGHCONTRASTW hc; ZeroMemory(&hc, sizeof hc); hc.cbSize = sizeof hc;
  g.highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
  if (g.highContrast) {
    g.activeBg = GetSysColor(COLOR_HIGHLIGHT); g.activeFg = GetSysColor(COLOR_HIGHLIGHTTEXT);
    g.alarmBg = g.activeBg; g.alarmFg = g.activeFg; g.okFg = GetSysColor(COLOR_WINDOWTEXT);
  } else {                                      // all >= 7:1 against white text
    g.activeBg = RGB(0, 70, 150); g.activeFg = RGB(255, 255, 255);
    g.alarmBg = RGB(160, 0, 30); g.alarmFg = RGB(255, 255, 255); g.okFg = RGB(0, 90, 30);
  }
}
void refreshPalette() { setPalette(); }
void initGfx() {
  HDC dc = GetDC(nullptr); int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96; if (dc) ReleaseDC(nullptr, dc);
  if (dpi < 96) dpi = 96;
  g.scale = dpi / 96.0;
  NONCLIENTMETRICSW ncm; ZeroMemory(&ncm, sizeof ncm); ncm.cbSize = sizeof ncm;
  bool ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0) != 0;
  if (!ok) { ZeroMemory(&ncm, sizeof ncm); ncm.cbSize = sizeof ncm - sizeof(int); ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0) != 0; }
  LOGFONTW lf; ZeroMemory(&lf, sizeof lf);
  if (ok) lf = ncm.lfMessageFont; else { lstrcpyW(lf.lfFaceName, L"Tahoma"); }
  int minPx = MulDiv(14, dpi, 96);                         // 14 px effective minimum (spec §11)
  if (std::abs(lf.lfHeight) < minPx) lf.lfHeight = -minPx;
  if (lf.lfHeight > 0) lf.lfHeight = -lf.lfHeight;
  g.fontPx = std::abs(lf.lfHeight);
  lf.lfWeight = FW_NORMAL; lf.lfQuality = CLEARTYPE_QUALITY;
  g.font = CreateFontIndirectW(&lf);
  lf.lfWeight = FW_BOLD; g.bold = CreateFontIndirectW(&lf);
  lf.lfWeight = FW_BOLD; lf.lfHeight = lf.lfHeight * 3 / 2; g.big = CreateFontIndirectW(&lf);
  LOGFONTW m = lf; m.lfHeight = -g.fontPx; m.lfWeight = FW_NORMAL; lstrcpyW(m.lfFaceName, L"Courier New"); g.mono = CreateFontIndirectW(&m);
  setPalette();
}
void freeGfx() { HFONT f[] = {g.font, g.bold, g.big, g.mono}; for (HFONT h : f) if (h) DeleteObject(h); g.font = g.bold = g.big = g.mono = nullptr; }
int effW(int px) { return (int)(px / g.scale + 0.5); }
int effH(int px) { return (int)(px / g.scale + 0.5); }

// ---- controls ---------------------------------------------------------------------------------------------------------------
HWND mkCtl(HWND parent, const wchar_t* cls, const std::string& text, DWORD style, int x, int y, int w, int h, int id, DWORD ex) {
  HWND c = CreateWindowExW(ex, cls, W(text).c_str(), style | WS_CHILD | WS_VISIBLE, x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
  if (c && g.font) SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
  return c;
}
std::string getText(HWND h) {
  int n = GetWindowTextLengthW(h); if (n <= 0) return std::string();
  std::wstring w((size_t)n + 1, L'\0'); GetWindowTextW(h, &w[0], n + 1); w.resize((size_t)n); return U8(w);
}
void setTextIf(HWND h, const std::string& s) { if (!h) return; if (getText(h) != s) SetWindowTextW(h, W(s).c_str()); }
void setEnabled(HWND h, bool on) { if (h && (IsWindowEnabled(h) != 0) != on) EnableWindow(h, on); }
HWND mkTooltip(HWND parent) {
  HWND t = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (t) { SendMessageW(t, WM_SETFONT, (WPARAM)g.font, TRUE); SendMessageW(t, TTM_SETMAXTIPWIDTH, 0, 360); }
  return t;
}
void tip(HWND tt, HWND target, const std::string& text) {
  if (!tt || !target) return;
  std::wstring w = W(text);
  TTTOOLINFOW ti; ZeroMemory(&ti, sizeof ti); ti.cbSize = TTTOOLINFOW_V1_SIZE; ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  ti.hwnd = GetParent(target); ti.uId = (UINT_PTR)target; ti.lpszText = const_cast<wchar_t*>(w.c_str());
  SendMessageW(tt, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}
std::string className(HWND h) { wchar_t b[64] = {}; GetClassNameW(h, b, 63); return U8(b); }
bool isTextField(HWND h) {
  if (!h) return false;
  std::string c = className(h);
  if (c == "Edit" || c.compare(0, 8, "RichEdit") == 0) { return true; }
  if (c == "ComboBox") return (GetWindowLongW(h, GWL_STYLE) & 3) == CBS_DROPDOWN;
  return false;
}
void comboFill(HWND cb, const std::vector<std::string>& items, int sel) {
  SendMessageW(cb, CB_RESETCONTENT, 0, 0);
  for (auto& s : items) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)W(s).c_str());
  if (sel >= 0 && sel < (int)items.size()) SendMessageW(cb, CB_SETCURSEL, (WPARAM)sel, 0);
}
int comboSel(HWND cb) { return (int)SendMessageW(cb, CB_GETCURSEL, 0, 0); }
void listFill(HWND lb, const std::vector<std::string>& items, int sel) {
  SendMessageW(lb, WM_SETREDRAW, FALSE, 0); SendMessageW(lb, LB_RESETCONTENT, 0, 0);
  for (auto& s : items) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)W(s).c_str());
  if (sel >= 0 && sel < (int)items.size()) SendMessageW(lb, LB_SETCURSEL, (WPARAM)sel, 0);
  SendMessageW(lb, WM_SETREDRAW, TRUE, 0); InvalidateRect(lb, nullptr, TRUE);
}

// ---- owner-draw buttons -----------------------------------------------------------------------------------------------------
void setBtnState(HWND b, BtnState s) {
  if ((BtnState)GetWindowLongPtrW(b, GWLP_USERDATA) != s) { SetWindowLongPtrW(b, GWLP_USERDATA, (LONG_PTR)s); InvalidateRect(b, nullptr, FALSE); }
}
void drawOwnerButton(const DRAWITEMSTRUCT* d) {
  HDC dc = d->hDC; RECT rc = d->rcItem;
  bool pressed = (d->itemState & ODS_SELECTED) != 0, dis = (d->itemState & ODS_DISABLED) != 0, focus = (d->itemState & ODS_FOCUS) != 0;
  BtnState st = (BtnState)GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA);
  COLORREF bg = GetSysColor(COLOR_BTNFACE), fg = GetSysColor(COLOR_BTNTEXT);
  if (dis) fg = GetSysColor(COLOR_GRAYTEXT);
  else if (st == BS_ACTIVE_) { bg = g.activeBg; fg = g.activeFg; }
  else if (st == BS_ALARM_) { bg = g.alarmBg; fg = g.alarmFg; }
  HBRUSH br = CreateSolidBrush(bg); FillRect(dc, &rc, br); DeleteObject(br);
  DrawEdge(dc, &rc, pressed ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
  RECT in = rc; InflateRect(&in, -4, -3);
  wchar_t buf[256]; int n = GetWindowTextW(d->hwndItem, buf, 255); buf[n > 0 ? n : 0] = 0;
  HFONT old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(d->hwndItem, WM_GETFONT, 0, 0));
  SetBkMode(dc, TRANSPARENT); SetTextColor(dc, fg);
  RECT calc = in; DrawTextW(dc, buf, -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
  int th = calc.bottom - calc.top; RECT tr = in; tr.top = in.top + ((in.bottom - in.top) - th) / 2; if (pressed) OffsetRect(&tr, 1, 1);
  DrawTextW(dc, buf, -1, &tr, DT_CENTER | DT_WORDBREAK);
  SelectObject(dc, old);
  if (focus) {                                  // always-visible focus: 2 px frame in the text color + dotted rect
    HPEN pen = CreatePen(PS_SOLID, 2, fg); HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
    RECT fr = rc; InflateRect(&fr, -5, -5); DrawFocusRect(dc, &fr);
  }
}

// ---- ListView -----------------------------------------------------------------------------------------------------------------
static std::map<HWND, std::string>& tableSigs() { static std::map<HWND, std::string> m; return m; }
HWND mkTable(HWND parent, int x, int y, int w, int h, int id, const std::vector<Column>& cols, bool checkboxes) {
  HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
  if (!lv) return nullptr;
  SendMessageW(lv, WM_SETFONT, (WPARAM)g.font, TRUE);
  DWORD ex = LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | (checkboxes ? LVS_EX_CHECKBOXES : 0);
  SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, ex, ex);
  for (size_t i = 0; i < cols.size(); ++i) {
    std::wstring t = W(cols[i].title); LVCOLUMNW c; ZeroMemory(&c, sizeof c);
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM; c.pszText = const_cast<wchar_t*>(t.c_str()); c.cx = cols[i].width; c.iSubItem = (int)i;
    SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
  }
  return lv;
}
void tableSet(HWND lv, const std::vector<std::vector<std::string>>& rows) {
  std::string sig; for (auto& r : rows) { for (auto& c : r) { sig += c; sig += '\x1f'; } sig += '\x1e'; }
  auto& sigs = tableSigs(); auto it = sigs.find(lv);
  if (it != sigs.end() && it->second == sig) return;
  sigs[lv] = sig;
  int sel = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
  SendMessageW(lv, WM_SETREDRAW, FALSE, 0); SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
  for (size_t i = 0; i < rows.size(); ++i) {
    for (size_t c = 0; c < rows[i].size(); ++c) {
      std::wstring t = W(rows[i][c]); LVITEMW it2; ZeroMemory(&it2, sizeof it2);
      it2.mask = LVIF_TEXT; it2.iItem = (int)i; it2.iSubItem = (int)c; it2.pszText = const_cast<wchar_t*>(t.c_str());
      if (c == 0) SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it2); else SendMessageW(lv, LVM_SETITEMTEXTW, i, (LPARAM)&it2);
    }
  }
  if (sel >= 0 && sel < (int)rows.size()) { LVITEMW s; ZeroMemory(&s, sizeof s); s.stateMask = LVIS_SELECTED | LVIS_FOCUSED; s.state = LVIS_SELECTED | LVIS_FOCUSED; SendMessageW(lv, LVM_SETITEMSTATE, (WPARAM)sel, (LPARAM)&s); }
  SendMessageW(lv, WM_SETREDRAW, TRUE, 0); InvalidateRect(lv, nullptr, TRUE);
}
int tableSel(HWND lv) { return (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED); }

// ---- frames -----------------------------------------------------------------------------------------------------------------------
void paintFrame(HDC dc, const RECT& rc, const Frame* f) {
  int W_ = rc.right - rc.left, H_ = rc.bottom - rc.top; if (W_ <= 0 || H_ <= 0) return;
  HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
  if (!f || f->w <= 0 || f->h <= 0 || f->px.size() < (size_t)f->w * (size_t)f->h) { FillRect(dc, &rc, black); return; }
  int dw = W_, dh = (int)((long long)W_ * f->h / f->w);
  if (dh > H_) { dh = H_; dw = (int)((long long)H_ * f->w / f->h); }
  int dx = rc.left + (W_ - dw) / 2, dy = rc.top + (H_ - dh) / 2;
  RECT b1 = {rc.left, rc.top, rc.right, dy}, b2 = {rc.left, dy + dh, rc.right, rc.bottom}, b3 = {rc.left, dy, dx, dy + dh}, b4 = {dx + dw, dy, rc.right, dy + dh};
  if (b1.bottom > b1.top) FillRect(dc, &b1, black);
  if (b2.bottom > b2.top) FillRect(dc, &b2, black);
  if (b3.right > b3.left) FillRect(dc, &b3, black);
  if (b4.right > b4.left) FillRect(dc, &b4, black);
  BITMAPINFO bi; ZeroMemory(&bi, sizeof bi);
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = f->w; bi.bmiHeader.biHeight = -f->h;
  bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(dc, COLORONCOLOR);
  StretchDIBits(dc, dx, dy, dw, dh, 0, 0, f->w, f->h, f->px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}
bool frameDiffers(const app::FramePtr& a, const app::FramePtr& b) { return a.get() != b.get(); }

// ---- FrameView / Meter classes --------------------------------------------------------------------------------------------------
struct FrameViewData { app::FramePtr f; };
static LRESULT CALLBACK FrameViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  FrameViewData* d = (FrameViewData*)GetWindowLongPtrW(h, GWLP_USERDATA);
  switch (m) {
    case WM_CREATE: SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) new FrameViewData()); return 0;
    case WM_NCDESTROY: delete d; SetWindowLongPtrW(h, GWLP_USERDATA, 0); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
      paintFrame(dc, rc, d ? d->f.get() : nullptr); EndPaint(h, &ps); return 0;
    }
  }
  return DefWindowProcW(h, m, w, l);
}
HWND mkFrameView(HWND parent, int x, int y, int w, int h) {
  return CreateWindowExW(WS_EX_CLIENTEDGE, L"SSFrameView", L"", WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}
void frameViewSet(HWND v, const app::FramePtr& f) {
  FrameViewData* d = (FrameViewData*)GetWindowLongPtrW(v, GWLP_USERDATA); if (!d) return;
  if (d->f.get() != f.get()) { d->f = f; InvalidateRect(v, nullptr, FALSE); }
}

struct MeterData { float rms[2] = {kDbSilence, kDbSilence}, peak[2] = {kDbSilence, kDbSilence}; bool nearFull = false, clip = false; };
void drawMeterBars(HDC dc, const RECT& rc, const float rms[2], const float peak[2]) {
  int w = rc.right - rc.left, h = rc.bottom - rc.top; if (w < 8 || h < 6) return;
  HBRUSH back = GetSysColorBrush(COLOR_WINDOW); FillRect(dc, &rc, back);
  HBRUSH fill = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT)); HBRUSH frame = GetSysColorBrush(COLOR_WINDOWTEXT);
  int bh = (h - 3) / 2;
  for (int ch = 0; ch < 2; ++ch) {
    RECT bar = {rc.left, rc.top + ch * (bh + 1), rc.right, rc.top + ch * (bh + 1) + bh};
    FrameRect(dc, &bar, frame);
    auto pos = [&](float db) { float t = (db + 60.f) / 60.f; if (t < 0) t = 0; if (t > 1) t = 1; return (int)(t * (w - 2)); };
    RECT f = {bar.left + 1, bar.top + 1, bar.left + 1 + pos(rms[ch]), bar.bottom - 1}; if (f.right > f.left) FillRect(dc, &f, fill);
    int pk = bar.left + 1 + pos(peak[ch]); HPEN pen = CreatePen(PS_SOLID, 2, GetSysColor(COLOR_WINDOWTEXT)); HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, pk, bar.top, nullptr); LineTo(dc, pk, bar.bottom); SelectObject(dc, op); DeleteObject(pen);
    int m6 = bar.left + 1 + pos(-6.f); HPEN p2 = CreatePen(PS_DOT, 1, GetSysColor(COLOR_GRAYTEXT)); op = SelectObject(dc, p2);   // -6 dBFS target
    MoveToEx(dc, m6, bar.top, nullptr); LineTo(dc, m6, bar.bottom); SelectObject(dc, op); DeleteObject(p2);
  }
  DeleteObject(fill);
}
static LRESULT CALLBACK MeterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  MeterData* d = (MeterData*)GetWindowLongPtrW(h, GWLP_USERDATA);
  switch (m) {
    case WM_CREATE: SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) new MeterData()); return 0;
    case WM_NCDESTROY: delete d; SetWindowLongPtrW(h, GWLP_USERDATA, 0); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
      HBRUSH bk = GetSysColorBrush(COLOR_WINDOW); FillRect(dc, &rc, bk);
      if (d) {
        int textW = S(190); RECT bars = rc; bars.right = rc.right - textW; if (bars.right < bars.left + 20) bars.right = rc.right;
        InflateRect(&bars, -1, -1); drawMeterBars(dc, bars, d->rms, d->peak);
        RECT tr = rc; tr.left = bars.right + S(6);
        std::string t = "L " + fmtDb(d->rms[0]) + "  R " + fmtDb(d->rms[1]);
        if (d->clip) t += "  [CLIPPING]"; else if (d->nearFull) t += "  [NEAR FULL SCALE]"; else t += "  [OK]";
        SelectObject(dc, g.font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        std::wstring wt = W(t); DrawTextW(dc, wt.c_str(), -1, &tr, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      }
      EndPaint(h, &ps); return 0;
    }
  }
  return DefWindowProcW(h, m, w, l);
}
HWND mkMeter(HWND parent, int x, int y, int w, int h) {
  return CreateWindowExW(WS_EX_CLIENTEDGE, L"SSMeter", L"", WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}
void meterSet(HWND mw, const float rms[2], const float peak[2], bool nearFull, bool clip) {
  MeterData* d = (MeterData*)GetWindowLongPtrW(mw, GWLP_USERDATA); if (!d) return;
  bool ch = d->nearFull != nearFull || d->clip != clip;
  for (int i = 0; i < 2; ++i) { if (std::fabs(d->rms[i] - rms[i]) > 0.2f || std::fabs(d->peak[i] - peak[i]) > 0.2f) ch = true; d->rms[i] = rms[i]; d->peak[i] = peak[i]; }
  d->nearFull = nearFull; d->clip = clip;
  if (ch) InvalidateRect(mw, nullptr, FALSE);
}
void registerControlClasses(HINSTANCE hi) {
  WNDCLASSW wc; ZeroMemory(&wc, sizeof wc); wc.hInstance = hi; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpfnWndProc = FrameViewProc; wc.lpszClassName = L"SSFrameView"; RegisterClassW(&wc);
  wc.lpfnWndProc = MeterProc; wc.lpszClassName = L"SSMeter"; RegisterClassW(&wc);
}

// ---- dialogs ------------------------------------------------------------------------------------------------------------------------
bool openFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter, std::wstring& out, bool multi, std::vector<std::wstring>* many) {
  std::vector<wchar_t> buf(32768, 0);
  if (!multi && !out.empty()) lstrcpynW(buf.data(), out.c_str(), (int)buf.size());
  OPENFILENAMEW o; ZeroMemory(&o, sizeof o); o.lStructSize = sizeof o; o.hwndOwner = owner; o.lpstrFilter = filter; o.lpstrFile = buf.data(); o.nMaxFile = (DWORD)buf.size();
  o.lpstrTitle = title; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | (multi ? (OFN_ALLOWMULTISELECT | OFN_EXPLORER) : 0);
  if (!GetOpenFileNameW(&o)) return false;
  if (multi && many) {
    many->clear(); const wchar_t* p = buf.data(); std::wstring dir = p; p += dir.size() + 1;
    if (!*p) many->push_back(dir);
    else while (*p) { std::wstring f = p; many->push_back(dir + L"\\" + f); p += f.size() + 1; }
    if (!many->empty()) out = many->front();
  } else out = buf.data();
  return true;
}
bool saveFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter, const wchar_t* defExt, std::wstring& out) {
  std::vector<wchar_t> buf(4096, 0); lstrcpynW(buf.data(), out.c_str(), (int)buf.size());
  OPENFILENAMEW o; ZeroMemory(&o, sizeof o); o.lStructSize = sizeof o; o.hwndOwner = owner; o.lpstrFilter = filter; o.lpstrFile = buf.data(); o.nMaxFile = (DWORD)buf.size();
  o.lpstrTitle = title; o.lpstrDefExt = defExt; o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&o)) return false; out = buf.data(); return true;
}
bool pickFolder(HWND owner, const wchar_t* title, std::wstring& out) {
  BROWSEINFOW bi; ZeroMemory(&bi, sizeof bi); bi.hwndOwner = owner; bi.lpszTitle = title; bi.ulFlags = BIF_RETURNONLYFSDIRS | 0x0040 /*BIF_NEWDIALOGSTYLE*/;
  LPITEMIDLIST pidl = SHBrowseForFolderW(&bi); if (!pidl) return false;
  wchar_t path[MAX_PATH * 2] = {}; bool ok = SHGetPathFromIDListW(pidl, path) != 0; CoTaskMemFree(pidl);
  if (ok) out = path; return ok;
}
int msgBox(HWND owner, const std::string& text, const std::string& caption, UINT flags) {
  return MessageBoxW(owner, W(text).c_str(), W(caption).c_str(), flags | MB_SETFOREGROUND);
}

// ---- notices / actions ---------------------------------------------------------------------------------------------------------------
static std::mutex gNoticeMu; static std::string gNotice; static bool gNoticeErr = false;
void postNotice(const std::string& text, bool isError) {
  { std::lock_guard<std::mutex> l(gNoticeMu); gNotice = text; gNoticeErr = isError; }
  if (A.mainWnd) PostMessageW(A.mainWnd, WM_SS_NOTICE, 0, 0);
}
std::string takeNotice(bool* isError) { std::lock_guard<std::mutex> l(gNoticeMu); if (isError) *isError = gNoticeErr; return gNotice; }
void act(ActionId id, double value, int target, ActionSource src) {
  if (!A.ctl) return;
  Action a; a.id = id; a.source = src; a.value = value; a.target = target;
  std::string nm = midiActionName(id);
  A.ctl->submit(a, [nm](const ActionResult& r) { if (!r.accepted) postNotice(nm + " rejected: " + r.reason, true); });
}
Status waitStatus(std::future<Status>& f, int ms) {
  if (!f.valid()) return Status::Fail(Err::InternalFault, "no result");
  if (f.wait_for(std::chrono::milliseconds(ms)) != std::future_status::ready) return Status::Fail(Err::InternalFault, "operation timed out");
  return f.get();
}

}} // namespace ss::ui
