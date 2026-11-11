#include "ui/main_window.h"
#include "ui/pages.h"
#include "ui/identify.h"
#include "app/version.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <objbase.h>
#include <shlobj.h>

namespace ss { namespace ui {
namespace {
enum { IDM_NEW = 2001, IDM_OPEN, IDM_SAVE, IDM_SAVEAS, IDM_EXPORT, IDM_IMPORT, IDM_EXIT, IDM_HELP = 2010, IDM_KEYS, IDM_ABOUT, IDM_WALK, IDM_MIRROR = 2020, IDM_IDENT };
enum { ID_TAB = 1, ID_VIS = 3001, ID_LIGHT, ID_EMER, ID_REST, ID_PAUSE, ID_RESUME, ID_NEXT, ID_BRI, ID_INT };
const UINT_PTR kTimer = 77;

// ---- top status bar ------------------------------------------------------------------------------------------------------
struct Seg { std::string text; int state = 0; double weight = 1; bool meter = false; };
struct TopState { std::vector<Seg> row1, row2; float rms[2] = {kDbSilence, kDbSilence}, peak[2] = {kDbSilence, kDbSilence}; std::string meterTag; bool compact = false; std::string sig; } gTop;

LRESULT CALLBACK TopProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps; HDC wdc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
      HDC dc = CreateCompatibleDC(wdc); HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(dc, bmp);
      FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE)); SelectObject(dc, g.font); SetBkMode(dc, TRANSPARENT);
      int rowH = rc.bottom / 2;
      for (int row = 0; row < 2; ++row) {
        auto& segs = row == 0 ? gTop.row1 : gTop.row2; double tot = 0; for (auto& s : segs) tot += s.weight; int x = 0;
        for (size_t i = 0; i < segs.size(); ++i) {
          auto& s = segs[i]; int sw = (i + 1 == segs.size()) ? rc.right - x : (int)(rc.right * s.weight / tot);
          RECT r = {x, row * rowH, x + sw, (row + 1) * rowH};
          COLORREF fg = GetSysColor(COLOR_BTNTEXT);
          if (s.state == BS_ACTIVE_) { HBRUSH b = CreateSolidBrush(g.activeBg); FillRect(dc, &r, b); DeleteObject(b); fg = g.activeFg; }
          else if (s.state == BS_ALARM_) { HBRUSH b = CreateSolidBrush(g.alarmBg); FillRect(dc, &r, b); DeleteObject(b); fg = g.alarmFg; }
          SetTextColor(dc, fg);
          RECT tr = r; tr.left += S(6); tr.right -= S(4);
          if (s.meter && !gTop.compact) {
            int mw = S(64), tagW = S(66); RECT mb = {r.right - tagW - mw - S(6), r.top + S(3), r.right - tagW - S(6), r.bottom - S(3)};
            if (mb.left > r.left + S(120)) { drawMeterBars(dc, mb, gTop.rms, gTop.peak); RECT tg = {mb.right + S(4), r.top, r.right, r.bottom};
              std::wstring t = W(gTop.meterTag); DrawTextW(dc, t.c_str(), -1, &tg, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); tr.right = mb.left - S(4); }
          }
          std::wstring t = W(s.text); DrawTextW(dc, t.c_str(), -1, &tr, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
          HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW)); HGDIOBJ op = SelectObject(dc, pen); MoveToEx(dc, r.right - 1, r.top + 2, nullptr); LineTo(dc, r.right - 1, r.bottom - 2); SelectObject(dc, op); DeleteObject(pen);
          x += sw;
        }
      }
      HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW)); HGDIOBJ op = SelectObject(dc, pen); MoveToEx(dc, 0, rc.bottom - 1, nullptr); LineTo(dc, rc.right, rc.bottom - 1); MoveToEx(dc, 0, rowH, nullptr); LineTo(dc, rc.right, rowH); SelectObject(dc, op); DeleteObject(pen);
      BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY); SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc); EndPaint(h, &ps); return 0;
    }
  }
  return DefWindowProcW(h, m, w, l);
}

std::string newUuid() {
  GUID gd; CoCreateGuid(&gd); char b[64]; snprintf(b, sizeof b, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", gd.Data1, gd.Data2, gd.Data3, gd.Data4[0], gd.Data4[1], gd.Data4[2], gd.Data4[3], gd.Data4[4], gd.Data4[5], gd.Data4[6], gd.Data4[7]); return b;
}
show::ShowDocument newShow() {
  show::ShowDocument d; d.uuid = newUuid(); d.uuid[14] = '4'; d.name = "Untitled show";
  const auto& pack = show::rendererPack();
  for (size_t i = 0; i < pack.size() && i < 8; ++i) { show::Scene s; s.id = "scene" + std::to_string(i + 1); s.name = pack[i].id; s.rendererType = pack[i].id; d.scenes.push_back(s); }
  show::Output a; a.logicalId = "main"; a.role = show::OutputRole::Audience; show::Output p; p.logicalId = "program"; p.role = show::OutputRole::Program;
  show::Output c; c.logicalId = "controls"; c.role = show::OutputRole::Controls; c.overlay.text = false; d.outputs = {a, p, c}; return d;
}
std::string defaultDocsDir() { wchar_t p[MAX_PATH] = {}; if (SUCCEEDED(SHGetFolderPathW(nullptr, 0x0005 /*CSIDL_PERSONAL*/, nullptr, 0, p))) return U8(p); return A.dataDir; }
} // namespace

// ==================================================================================================================================
bool MainWindow::create(HINSTANCE hi, int nCmdShow) {
  WNDCLASSW wc; ZeroMemory(&wc, sizeof wc); wc.lpfnWndProc = &MainWindow::Proc; wc.hInstance = hi; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"StageSignalMain"; wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); RegisterClassW(&wc);
  WNDCLASSW tc; ZeroMemory(&tc, sizeof tc); tc.lpfnWndProc = TopProc; tc.hInstance = hi; tc.hCursor = LoadCursorW(nullptr, IDC_ARROW); tc.lpszClassName = L"SSTopBar"; RegisterClassW(&tc);
  registerControlClasses(hi); outs_.registerClass(hi);
  HMENU mb = CreateMenu(), fm = CreatePopupMenu(), vm = CreatePopupMenu(), hm = CreatePopupMenu();
  AppendMenuW(fm, MF_STRING, IDM_NEW, L"&New show"); AppendMenuW(fm, MF_STRING, IDM_OPEN, L"&Open show...");
  AppendMenuW(fm, MF_STRING, IDM_SAVE, L"&Save\tCtrl+S"); AppendMenuW(fm, MF_STRING, IDM_SAVEAS, L"Save &as...");
  AppendMenuW(fm, MF_SEPARATOR, 0, nullptr); AppendMenuW(fm, MF_STRING, IDM_EXPORT, L"&Export portable bundle..."); AppendMenuW(fm, MF_STRING, IDM_IMPORT, L"&Import portable bundle...");
  AppendMenuW(fm, MF_SEPARATOR, 0, nullptr); AppendMenuW(fm, MF_STRING, IDM_EXIT, L"E&xit");
  AppendMenuW(vm, MF_STRING, IDM_MIRROR, L"&Mirror control window"); AppendMenuW(vm, MF_STRING, IDM_IDENT, L"&Identify displays");
  AppendMenuW(hm, MF_STRING, IDM_HELP, L"&Help topics\tF1"); AppendMenuW(hm, MF_STRING, IDM_WALK, L"&First-show walkthrough"); AppendMenuW(hm, MF_STRING, IDM_KEYS, L"&Keyboard map"); AppendMenuW(hm, MF_STRING, IDM_ABOUT, L"&About and licenses");
  AppendMenuW(mb, MF_POPUP, (UINT_PTR)fm, L"&File"); AppendMenuW(mb, MF_POPUP, (UINT_PTR)vm, L"&View"); AppendMenuW(mb, MF_POPUP, (UINT_PTR)hm, L"&Help");
  RECT want = {0, 0, S(800), S(600)}; AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_CONTROLPARENT);
  RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  int ww = std::min<int>(want.right - want.left, wa.right - wa.left), wh = std::min<int>(want.bottom - want.top, wa.bottom - wa.top);
  int x = wa.left + ((wa.right - wa.left) - ww) / 2, y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
  hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"StageSignalMain", L"StageSignal", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, ww, wh, nullptr, mb, hi, this);
  if (!hwnd_) return false;
  A.mainWnd = hwnd_; A.main = this;
  SendMessageW(hwnd_, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);   // focus rectangles always visible
  ShowWindow(hwnd_, nCmdShow); UpdateWindow(hwnd_);
  SetTimer(hwnd_, kTimer, 100, nullptr);
  tick();
  return true;
}

LRESULT CALLBACK MainWindow::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  MainWindow* t = (MainWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (m == WM_NCCREATE) { t = (MainWindow*)((CREATESTRUCTW*)l)->lpCreateParams; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)t); t->hwnd_ = h; }
  if (t && m != WM_NCCREATE) return t->proc(m, w, l);
  return DefWindowProcW(h, m, w, l);
}

LRESULT MainWindow::proc(UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: {
      top_ = CreateWindowExW(0, L"SSTopBar", L"Status", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
      tab_ = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | TCS_FOCUSONBUTTONDOWN | TCS_MULTILINE, 0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)ID_TAB, GetModuleHandleW(nullptr), nullptr);
      SendMessageW(tab_, WM_SETFONT, (WPARAM)g.font, TRUE);
      pages_.push_back(makeLivePage()); pages_.push_back(makeSetupPage()); pages_.push_back(makeMediaPage()); pages_.push_back(makeMidiLightPage()); pages_.push_back(makeRecordPage()); pages_.push_back(makeDiagPage());
      for (size_t i = 0; i < pages_.size(); ++i) { std::wstring t = W(pages_[i]->title); TCITEMW ti; ZeroMemory(&ti, sizeof ti); ti.mask = TCIF_TEXT; ti.pszText = const_cast<wchar_t*>(t.c_str()); SendMessageW(tab_, TCM_INSERTITEMW, i, (LPARAM)&ti); pages_[i]->create(hwnd_); }
      msg_ = mkCtl(hwnd_, L"STATIC", "", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 10, 10, 0);
      tt_ = mkTooltip(hwnd_); buildStrip();
      return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)l; mm->ptMinTrackSize.x = S(480); mm->ptMinTrackSize.y = S(360); return 0; }
    case WM_TIMER: if (w == kTimer) tick(); return 0;
    case WM_COMMAND: onCommand((int)LOWORD(w), (int)HIWORD(w), (HWND)l); return 0;
    case WM_NOTIFY: { NMHDR* nh = (NMHDR*)l; if (nh && nh->idFrom == ID_TAB && nh->code == TCN_SELCHANGE) showTab((int)SendMessageW(tab_, TCM_GETCURSEL, 0, 0)); return 0; }
    case WM_DRAWITEM: if (w != 0) { drawOwnerButton((const DRAWITEMSTRUCT*)l); return TRUE; } break;
    case WM_CTLCOLORSTATIC: { HDC dc = (HDC)w; SetBkMode(dc, TRANSPARENT); SetTextColor(dc, GetSysColor(COLOR_BTNTEXT)); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
    case WM_HSCROLL: {
      HWND c = (HWND)l; if (c == tbBri_) act(ActionId::Brightness, SendMessageW(c, TBM_GETPOS, 0, 0) / 100.0, -1);
      else if (c == tbInt_) act(ActionId::Intensity, SendMessageW(c, TBM_GETPOS, 0, 0) / 100.0, -1); return 0;
    }
    case WM_SS_NOTICE: { bool err = false; std::string t = takeNotice(&err); setTextIf(msg_, (err ? "Notice: " : "") + t); noticeAt_ = GetTickCount(); return 0; }
    case WM_SS_KEY: handleKey(w, l); return 0;
    case WM_SYSCOLORCHANGE: case WM_SETTINGCHANGE: refreshPalette(); InvalidateRect(hwnd_, nullptr, TRUE); return 0;
    case WM_CLOSE: {
      auto st = A.ctl->status(); auto rs = A.ctl->recordingStatus();
      bool live = st.liveState != BroadcastState::Off || rs.state == RecState::Recording || st.dmxArmed;
      if (live && msgBox(hwnd_, "Recording, broadcast or DMX output is active. Exiting stops them in order (disarm, broadcast, recording).\n\nExit now?", "Exit StageSignal", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 0;
      if (!confirmDiscard()) return 0;
      DestroyWindow(hwnd_); return 0;
    }
    case WM_DESTROY: KillTimer(hwnd_, kTimer); outs_.closeAll(); if (mirror_ && mirror_->hwnd) DestroyWindow(mirror_->hwnd); PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(hwnd_, m, w, l);
}

void MainWindow::buildStrip() {
  auto mk = [&](const char* t, int id, const char* tipText) { HWND b = mkCtl(hwnd_, L"BUTTON", t, BS_OWNERDRAW | WS_TABSTOP, 0, 0, 10, 10, id); tip(tt_, b, tipText); return b; };
  bVis_ = mk("Visual BO", ID_VIS, "Visual Blackout: latches the final audience picture to black (F8 = combined Emergency Blackout)");
  bLight_ = mk("Light BO", ID_LIGHT, "Lighting Blackout: latches lighting channels to their blackout values");
  bEmer_ = mk("EMERGENCY", ID_EMER, "Emergency Blackout (F8): visual + lighting. Audio recording is not affected. Latched until Restore");
  bRestore_ = mk("Restore", ID_REST, "Restore (Shift+F8): explicit restore of the state before the blackout");
  bPause_ = mk("Pause", ID_PAUSE, "Pause automation (F6)"); bResume_ = mk("Resume", ID_RESUME, "Resume automation (F7)"); bNext_ = mk("Next", ID_NEXT, "Next scene (F5)");
  lBri_ = mkCtl(hwnd_, L"STATIC", "Bright", SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, 0); lInt_ = mkCtl(hwnd_, L"STATIC", "Intens", SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, 0);
  tbBri_ = mkCtl(hwnd_, TRACKBAR_CLASSW, "", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 0, 0, 10, 10, ID_BRI); tbInt_ = mkCtl(hwnd_, TRACKBAR_CLASSW, "", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 0, 0, 10, 10, ID_INT);
  for (HWND t : {tbBri_, tbInt_}) { SendMessageW(t, TBM_SETRANGE, TRUE, MAKELONG(0, 100)); SendMessageW(t, TBM_SETPOS, TRUE, 100); SendMessageW(t, TBM_SETPAGESIZE, 0, 10); SendMessageW(t, TBM_SETLINESIZE, 0, 5); }
  tip(tt_, tbBri_, "Brightness of the target output (arrow keys adjust)"); tip(tt_, tbInt_, "Intensity of the target output / lighting (arrow keys adjust)");
}

void MainWindow::layout() {
  if (!top_) return;
  RECT rc; GetClientRect(hwnd_, &rc); cw_ = rc.right; ch_ = rc.bottom;
  int topH = S(48), stripH = S(60), msgH = S(24);
  compact_ = cw_ < S(800) || ch_ < S(600); gTop.compact = compact_;
  MoveWindow(top_, 0, 0, cw_, topH, TRUE);
  int tabH = std::max(0, ch_ - topH - stripH - msgH);
  MoveWindow(tab_, 0, topH, cw_, tabH, TRUE);
  RECT disp = {0, topH, cw_, topH + tabH}; SendMessageW(tab_, TCM_ADJUSTRECT, FALSE, (LPARAM)&disp);
  for (auto& p : pages_) p->place(disp);
  MoveWindow(msg_, S(8), topH + tabH + S(2), cw_ - S(16), msgH - S(4), TRUE);
  int sy = ch_ - stripH, bh = S(40), by = sy + (stripH - bh) / 2, gap = S(compact_ ? 3 : 4), x = S(8);
  struct B { HWND h; int w; } bs[] = {{bVis_, 84}, {bLight_, 84}, {bEmer_, 100}, {bRestore_, 70}, {bPause_, 62}, {bResume_, 70}, {bNext_, 56}};
  for (auto& b : bs) { MoveWindow(b.h, x, by, S(b.w), bh, TRUE); x += S(b.w) + gap; }
  int lw = S(46), avail = cw_ - x - S(8), tw = std::max(S(40), (avail - 2 * lw - 2 * gap) / 2), th = S(32), ty = sy + (stripH - th) / 2;
  MoveWindow(lBri_, x, ty + S(6), lw, S(22), TRUE); x += lw; MoveWindow(tbBri_, x, ty, tw, th, TRUE); x += tw + gap;
  MoveWindow(lInt_, x, ty + S(6), lw, S(22), TRUE); x += lw; MoveWindow(tbInt_, x, ty, tw, th, TRUE);
  InvalidateRect(hwnd_, nullptr, TRUE);
  showTab(cur_);
}

void MainWindow::showTab(int i) {
  if (i < 0 || i >= (int)pages_.size()) return;
  bool changed = i != cur_ || !IsWindowVisible(pages_[(size_t)i]->hwnd);
  cur_ = i; SendMessageW(tab_, TCM_SETCURSEL, (WPARAM)i, 0);
  for (size_t k = 0; k < pages_.size(); ++k) ShowWindow(pages_[k]->hwnd, (int)k == i ? SW_SHOW : SW_HIDE);
  if (changed) pages_[(size_t)i]->activated();
  pages_[(size_t)i]->refresh();
}

void MainWindow::updateTopBar(const app::AppStatus& st) {
  TopState t; t.compact = compact_;
  const char* as[] = {"no device", "opening", "Running", "UNAVAILABLE", "DENIED", "DISCONNECTED"};
  Seg a; a.meter = true; a.weight = 0.60; a.text = "Audio: " + (st.audioDevice.empty() ? std::string("none selected") : st.audioDevice) + " - " + as[(int)st.audioState];
  if (st.audioLossPolicyEngaged && st.audioState != AudioState::Closed) a.text += " (LOST)";
  a.state = (st.audioState == AudioState::Unavailable || st.audioState == AudioState::Denied || st.audioState == AudioState::Disconnected) ? BS_ALARM_ : BS_NORMAL_;
  t.meterTag = st.possibleClipping ? "CLIP" : st.inputNearFullScale ? "NEAR FS" : "OK";
  if (st.possibleClipping) a.state = BS_ALARM_;
  for (int i = 0; i < 2; ++i) { t.rms[i] = st.meterRmsDb[i]; t.peak[i] = st.meterPeakDb[i]; }
  const char* mn[] = {"Auto", "Programmed", "Hybrid"}; Seg m; m.weight = 0.40; m.text = std::string("Mode: ") + mn[(int)st.mode];
  if (st.emergencyBlackout) { m.text += " | EMERGENCY BLACKOUT"; m.state = BS_ALARM_; } else if (st.visualBlackout || st.lightingBlackout) { m.text += st.visualBlackout ? " | VISUAL BLACKOUT" : " | LIGHT BLACKOUT"; m.state = BS_ALARM_; }
  else if (st.paused) { m.text += " | PAUSED"; m.state = BS_ACTIVE_; } else if (st.manualOverride) { m.text += " | OVERRIDE"; m.state = BS_ACTIVE_; } else if (st.idleActive) { m.text += " | IDLE"; m.state = BS_ACTIVE_; }
  t.row1 = {a, m};
  Seg b; b.weight = 0.38;
  b.text = (st.bpmValid ? fmt("BPM %.1f (%s, %d%%)", st.bpm, st.bpmLabel.c_str(), (int)std::lround(st.bpmConfidence * 100)) : std::string("BPM Unknown")) + "   Key: " + (st.keyValid ? st.keyName + " (" + st.keyLabel + ")" : "Unknown");
  Seg c; c.weight = 0.14; const char* cs[] = {"Off", "Opening", "Live", "Frozen", "LOST", "ERROR"}; c.text = std::string("Cam: ") + cs[(int)st.cameraState] + (st.cameraPublished ? " (pub)" : "");
  if (st.cameraState == CameraState::Lost || st.cameraState == CameraState::Error) c.state = BS_ALARM_;
  Seg d; d.weight = 0.13; d.text = st.dmxArmed ? "DMX: ARMED" : "DMX: Off"; if (st.dmxArmed) d.state = BS_ACTIVE_;
  Seg r; r.weight = 0.16;
  if (st.recState == RecState::Recording) { r.text = "REC " + fmtTime(st.recElapsedSec); r.state = BS_ALARM_; }
  else if (st.recState == RecState::Failed || st.recState == RecState::Recoverable) { r.text = std::string("REC: ") + recStateName(st.recState); r.state = BS_ALARM_; }
  else r.text = std::string("REC: ") + recStateName(st.recState);
  Seg v; v.weight = 0.19; v.text = "LIVE: " + st.liveText;
  if (st.liveState == BroadcastState::Live) { v.text = "LIVE: ON AIR"; v.state = BS_ALARM_; } else if (st.liveState == BroadcastState::Error) v.state = BS_ALARM_; else if (st.liveState != BroadcastState::Off) v.state = BS_ACTIVE_;
  t.row2 = {b, c, d, r, v};
  std::string sig; for (auto& s : t.row1) sig += s.text + std::to_string(s.state) + "|"; for (auto& s : t.row2) sig += s.text + std::to_string(s.state) + "|";
  for (int i = 0; i < 2; ++i) sig += fmt("%.0f,%.0f,", (double)t.rms[i], (double)t.peak[i]); sig += t.meterTag + (compact_ ? "c" : "n");
  if (sig != gTop.sig) { t.sig = sig; gTop = t; InvalidateRect(top_, nullptr, FALSE); }
}

void MainWindow::updateStrip(const app::AppStatus& st, const show::ShowSnapshot& sn) {
  setTextIf(bVis_, st.visualBlackout ? "Visual BO\r\nLATCHED" : "Visual BO"); setBtnState(bVis_, st.visualBlackout ? BS_ALARM_ : BS_NORMAL_);
  setTextIf(bLight_, st.lightingBlackout ? "Light BO\r\nLATCHED" : "Light BO"); setBtnState(bLight_, st.lightingBlackout ? BS_ALARM_ : BS_NORMAL_);
  setTextIf(bEmer_, st.emergencyBlackout ? "EMERGENCY\r\nLATCHED" : "EMERGENCY"); setBtnState(bEmer_, st.emergencyBlackout ? BS_ALARM_ : BS_NORMAL_);
  bool any = st.visualBlackout || st.lightingBlackout || st.emergencyBlackout; setEnabled(bRestore_, any); setBtnState(bRestore_, any ? BS_ACTIVE_ : BS_NORMAL_);
  setTextIf(bPause_, st.paused ? "Pause\r\nPAUSED" : "Pause"); setBtnState(bPause_, st.paused ? BS_ACTIVE_ : BS_NORMAL_);
  setBtnState(bResume_, BS_NORMAL_);
  HWND f = GetFocus(); int t = sn.target; const show::OutputState* os = nullptr;
  for (size_t i = 0; i < sn.outputs.size(); ++i) { if (t >= 0 ? (int)i == t : sn.outputs[i].role != show::OutputRole::Controls) { os = &sn.outputs[i]; break; } }
  if (os) { if (f != tbBri_) SendMessageW(tbBri_, TBM_SETPOS, TRUE, (LPARAM)std::lround(os->brightness * 100)); if (f != tbInt_) SendMessageW(tbInt_, TBM_SETPOS, TRUE, (LPARAM)std::lround(os->intensity * 100)); }
}

void MainWindow::tick() {
  if (!A.ctl) return;
  app::AppStatus st = A.ctl->status(); show::ShowSnapshot sn = A.ctl->showSnapshot();
  updateTopBar(st); updateStrip(st, sn);
  std::string ttl = "StageSignal - " + st.showName + (st.showDirty ? " *" : "") + (A.showPath.empty() ? "" : "") ; if (ttl != title_) { title_ = ttl; SetWindowTextW(hwnd_, W(ttl).c_str()); }
  DWORD now = GetTickCount();
  if (compact_) { setTextIf(msg_, fmt("Compact layout: effective area %dx%d is below the supported minimum 800x600; pages scroll. Use a larger window or lower display scaling.", effW(cw_), effH(ch_))); }
  else if (noticeAt_ && now - noticeAt_ > 9000) { noticeAt_ = 0; setTextIf(msg_, ""); }
  if (!noticeAt_ && !compact_) { std::string wtxt; for (auto& w : st.warnings) if (w.critical) { wtxt = "ALERT: " + w.text; break; } if (wtxt.empty() && !st.warnings.empty()) wtxt = "Warning: " + st.warnings[0].text; if (wtxt.empty() && st.recovered) wtxt = "Restored copy from crash recovery: DMX, recording and broadcast are disarmed."; setTextIf(msg_, wtxt); }
  if (cur_ >= 0 && cur_ < (int)pages_.size()) pages_[(size_t)cur_]->refresh();
  outs_.sync(); if (mirror_ && mirror_->hwnd) mirror_->tick();
}

void MainWindow::onCommand(int id, int code, HWND) {
  switch (id) {
    case ID_VIS: act(ActionId::VisualBlackout); break; case ID_LIGHT: act(ActionId::LightingBlackout); break; case ID_EMER: act(ActionId::EmergencyBlackout); break;
    case ID_REST: act(ActionId::RestoreBlackout); break; case ID_PAUSE: act(ActionId::PauseAutomation); break; case ID_RESUME: act(ActionId::ResumeAutomation); break; case ID_NEXT: act(ActionId::NextScene); break;
    case IDM_NEW: doNew(); break; case IDM_OPEN: doOpen(); break; case IDM_SAVE: doSave(false); break; case IDM_SAVEAS: doSave(true); break; case IDM_EXPORT: doExport(); break; case IDM_IMPORT: doImport(); break;
    case IDM_EXIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case IDM_HELP: showHelp(); break; case IDM_WALK: showHelp(0); break; case IDM_KEYS: showHelpTopic("Keyboard"); break; case IDM_ABOUT: showAbout(hwnd_); break;
    case IDM_IDENT: identifyDisplays(); break;
    case IDM_MIRROR: if (!mirror_) mirror_.reset(new MirrorWindow()); mirror_->show(hwnd_); break;
    case IDOK: case IDCANCEL: break;      // Enter/Esc in a field must never trigger anything
  }
  (void)code;
}

bool MainWindow::handleKey(WPARAM vk, LPARAM lp) {
  if (lp & (1 << 30)) return vk == VK_F1 || (vk >= VK_F5 && vk <= VK_F8);   // swallow auto-repeat of our keys
  bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
  switch (vk) {
    case VK_F1: showHelp(); return true;
    case VK_F5: act(ActionId::NextScene, 0, -1, ActionSource::Keyboard); return true;
    case VK_F6: act(ActionId::PauseAutomation, 0, -1, ActionSource::Keyboard); return true;
    case VK_F7: act(ActionId::ResumeAutomation, 0, -1, ActionSource::Keyboard); return true;
    case VK_F8: act(shift ? ActionId::RestoreBlackout : ActionId::EmergencyBlackout, 0, -1, ActionSource::Keyboard); return true;
  }
  if (ctrl && !alt && vk == 'S') { doSave(shift); return true; }
  if (!ctrl && !alt && vk >= '1' && vk <= '8' && !isTextField(GetFocus())) {       // pads are never triggered while typing
    auto sn = A.ctl->showSnapshot(); act(ActionId::SelectScene, sn.bank * 8 + (int)(vk - '1'), -1, ActionSource::Keyboard); return true;
  }
  return false;
}

bool MainWindow::preTranslate(MSG& m) {
  if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
    HWND root = GetAncestor(m.hwnd, GA_ROOT);
    if (root == hwnd_) {
      if (m.wParam == VK_TAB && GetKeyState(VK_CONTROL) < 0) { int n = (int)pages_.size(); showTab((cur_ + (GetKeyState(VK_SHIFT) < 0 ? n - 1 : 1)) % n); return true; }
      if (handleKey(m.wParam, m.lParam)) return true;
    }
  }
  HWND root = GetAncestor(m.hwnd, GA_ROOT);
  if (root && root != GetDesktopWindow()) {
    if (IsDialogMessageW(root, &m)) {
      if (root == hwnd_ && m.message == WM_KEYDOWN && m.wParam == VK_TAB) { HWND f = GetFocus(); for (auto& p : pages_) if (p->owns(f)) p->ensureVisible(f); }
      return true;
    }
  }
  return false;
}

bool MainWindow::confirmDiscard() {
  if (!A.ctl->showDirty()) return true;
  int r = msgBox(hwnd_, "The show has unsaved changes. Save before continuing?", "Unsaved show", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (r == IDCANCEL) return false; if (r == IDYES) return doSave(false); return true;
}
bool MainWindow::doSave(bool saveAs) {
  std::string path = A.showPath;
  if (saveAs || path.empty()) {
    std::wstring p = W(defaultDocsDir() + "\\" + "show.stagesignal");
    if (!saveFileDialog(hwnd_, L"Save show", L"StageSignal show\0*.stagesignal;*.json\0All files\0*.*\0", L"stagesignal", p)) return false; path = U8(p);
  }
  auto f = A.ctl->saveShow(path); Status s = waitStatus(f, 8000);
  if (!s.ok()) { postNotice("Save failed (previous file kept): " + s.detail, true); msgBox(hwnd_, "The show could not be saved: " + s.detail + "\nThe previous saved version is untouched.", "Save failed", MB_OK | MB_ICONERROR); return false; }
  A.showPath = path; postNotice("Saved " + path, false); return true;
}
void MainWindow::doNew() { if (!confirmDiscard()) return; auto f = A.ctl->applyDocument(newShow()); Status s = waitStatus(f); A.showPath.clear(); postNotice(s.ok() ? "New show created." : "New show failed: " + s.detail, !s.ok()); }
void MainWindow::doOpen() {
  if (!confirmDiscard()) return; std::wstring p;
  if (!openFileDialog(hwnd_, L"Open show", L"StageSignal show\0*.stagesignal;*.json\0All files\0*.*\0", p)) return;
  auto f = A.ctl->openShow(U8(p)); Status s = waitStatus(f, 15000);
  if (s.ok()) { A.showPath = U8(p); postNotice("Opened " + U8(p) + " (outputs, DMX, recording and broadcast stay disarmed).", false); }
  else { postNotice("Open failed; the current show keeps running: " + s.detail, true); msgBox(hwnd_, "The show could not be opened: " + s.detail + "\nThe current show is unchanged.", "Open failed", MB_OK | MB_ICONERROR); }
}
void MainWindow::doExport() {
  std::wstring p = L"show-portable.zip"; if (!saveFileDialog(hwnd_, L"Export portable bundle", L"ZIP archive\0*.zip\0", L"zip", p)) return;
  auto f = A.ctl->exportPortable(U8(p)); Status s = waitStatus(f, 60000); postNotice(s.ok() ? "Portable bundle exported (no secrets or local paths included)." : "Export failed: " + s.detail, !s.ok());
}
void MainWindow::doImport() {
  std::wstring p, d; if (!openFileDialog(hwnd_, L"Import portable bundle", L"ZIP archive\0*.zip\0", p)) return; if (!pickFolder(hwnd_, L"Destination folder (an empty or new folder)", d)) return;
  auto f = A.ctl->importPortable(U8(p), U8(d)); Status s = waitStatus(f, 60000); postNotice(s.ok() ? "Imported into " + U8(d) + ". Open the show file from there." : "Import failed: " + s.detail, !s.ok());
}

void MainWindow::screenshot(const std::wstring& path) {
  RECT rc; GetWindowRect(hwnd_, &rc); int w = rc.right - rc.left, h = rc.bottom - rc.top;
  HDC wdc = GetDC(hwnd_); HDC dc = CreateCompatibleDC(wdc); HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h); HGDIOBJ ob = SelectObject(dc, bmp);
  RECT cr; GetClientRect(hwnd_, &cr); FillRect(dc, &cr, (HBRUSH)GetStockObject(WHITE_BRUSH));
  { POINT o = {0, 0}; ClientToScreen(hwnd_, &o); HDC sdc = GetDC(nullptr); BitBlt(dc, 0, 0, cr.right, cr.bottom, sdc, o.x, o.y, SRCCOPY); ReleaseDC(nullptr, sdc); }   // screen copy (works under wine/Xvfb)
  BITMAPINFOHEADER bi; ZeroMemory(&bi, sizeof bi); bi.biSize = sizeof bi; bi.biWidth = cr.right; bi.biHeight = cr.bottom; bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB;
  int stride = ((cr.right * 3 + 3) / 4) * 4; std::vector<unsigned char> px((size_t)stride * cr.bottom); GetDIBits(dc, bmp, 0, cr.bottom, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
  BITMAPFILEHEADER fh; ZeroMemory(&fh, sizeof fh); fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof bi; fh.bfSize = (DWORD)(fh.bfOffBits + px.size());
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) { DWORD n; WriteFile(f, &fh, sizeof fh, &n, nullptr); WriteFile(f, &bi, sizeof bi, &n, nullptr); WriteFile(f, px.data(), (DWORD)px.size(), &n, nullptr); CloseHandle(f); }
  SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc); ReleaseDC(hwnd_, wdc);
}

int runMessageLoop(MainWindow& w) {
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) { if (w.preTranslate(m)) continue; TranslateMessage(&m); DispatchMessageW(&m); }
  return 0;
}

// ---- mirror window ----------------------------------------------------------------------------------------------------------------
void MirrorWindow::show(HWND owner) {
  if (hwnd) { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); return; }
  static bool reg = false;
  if (!reg) { WNDCLASSW wc; ZeroMemory(&wc, sizeof wc); wc.lpfnWndProc = &MirrorWindow::Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"SSMirror"; RegisterClassW(&wc); reg = true; }
  hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"SSMirror", L"StageSignal - mirror controls", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(520), S(330), nullptr, nullptr, GetModuleHandleW(nullptr), this);
  banner_ = mkCtl(hwnd, L"STATIC", "", SS_LEFT | SS_NOPREFIX, S(8), S(8), S(490), S(40), 0);
  lbl_ = mkCtl(hwnd, L"STATIC", "", SS_LEFT | SS_NOPREFIX, S(8), S(54), S(490), S(120), 0);
  bPrev_ = mkCtl(hwnd, L"BUTTON", "Prev scene", WS_TABSTOP, S(8), S(190), S(110), S(36), 11); bNext_ = mkCtl(hwnd, L"BUTTON", "Next scene", WS_TABSTOP, S(124), S(190), S(110), S(36), 12);
  bBlack_ = mkCtl(hwnd, L"BUTTON", "EMERGENCY BLACKOUT", WS_TABSTOP, S(240), S(190), S(160), S(36), 13); bRestore_ = mkCtl(hwnd, L"BUTTON", "Restore", WS_TABSTOP, S(406), S(190), S(92), S(36), 14);
  bAccept_ = mkCtl(hwnd, L"BUTTON", "Accept current state", WS_TABSTOP, S(8), S(236), S(190), S(36), 15);
  SendMessageW(hwnd, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  ShowWindow(hwnd, SW_SHOW); tick();
}
void MirrorWindow::send(ActionId id) {
  Action a; a.id = id; a.source = ActionSource::UI; a.expectRevision = haveBase_ ? baseRev_ : 0;
  A.ctl->submit(a, [](const ActionResult& r) { if (!r.accepted) postNotice("Mirror: " + r.reason, true); });
}
void MirrorWindow::tick() {
  if (!hwnd) return; auto sn = A.ctl->showSnapshot(); auto st = A.ctl->status();
  if (!haveBase_) { baseRev_ = sn.revision; haveBase_ = true; }
  bool conflict = sn.revision != baseRev_;
  setTextIf(banner_, conflict ? fmt("CONFLICT: the show state changed on another control surface (revision %llu -> %llu). Your actions here are refused until you accept the current state.", (unsigned long long)baseRev_, (unsigned long long)sn.revision) : "State in sync.");
  setTextIf(lbl_, fmt("Revision %llu | Mode %s | Scene %s | %s%s\r\nBPM %s | Key %s\r\nBlackout: visual %s, lighting %s, emergency %s\r\nDMX %s | REC %s | LIVE %s", (unsigned long long)sn.revision, st.mode == show::ShowMode::Auto ? "Auto" : st.mode == show::ShowMode::Programmed ? "Programmed" : "Hybrid", st.sceneId.c_str(),
    st.paused ? "PAUSED" : "running", st.manualOverride ? ", override" : "", st.bpmValid ? fmt("%.1f", st.bpm).c_str() : "Unknown", st.keyValid ? st.keyName.c_str() : "Unknown",
    st.visualBlackout ? "ON" : "off", st.lightingBlackout ? "ON" : "off", st.emergencyBlackout ? "ON" : "off", st.dmxText.c_str(), recStateName(st.recState), st.liveText.c_str()));
  setEnabled(bAccept_, conflict);
}
LRESULT CALLBACK MirrorWindow::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  MirrorWindow* t = (MirrorWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (m == WM_NCCREATE) { t = (MirrorWindow*)((CREATESTRUCTW*)l)->lpCreateParams; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)t); }
  if (t) switch (m) {
    case WM_COMMAND:
      switch (LOWORD(w)) { case 11: t->send(ActionId::PrevScene); break; case 12: t->send(ActionId::NextScene); break; case 13: t->send(ActionId::EmergencyBlackout); break; case 14: t->send(ActionId::RestoreBlackout); break;
        case 15: t->haveBase_ = false; t->tick(); break; case IDCANCEL: ShowWindow(h, SW_HIDE); break; }
      return 0;
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    case WM_CTLCOLORSTATIC: { HDC dc = (HDC)w; SetBkMode(dc, TRANSPARENT); SetTextColor(dc, GetSysColor(COLOR_BTNTEXT)); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
    case WM_NCDESTROY: t->hwnd = nullptr; break;
  }
  return DefWindowProcW(h, m, w, l);
}
}} // namespace ss::ui
