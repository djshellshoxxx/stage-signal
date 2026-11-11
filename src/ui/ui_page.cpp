#include "ui/ui_page.h"
#include <algorithm>

namespace ss { namespace ui {

static bool gPageClass = false;

HWND Page::create(HWND parent) {
  if (!gPageClass) {
    WNDCLASSW wc; ZeroMemory(&wc, sizeof wc); wc.lpfnWndProc = &Page::Proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"SSPage";
    RegisterClassW(&wc); gPageClass = true;
  }
  hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"SSPage", W(title).c_str(), WS_CHILD | WS_CLIPCHILDREN | WS_VSCROLL, 0, 0, 100, 100, parent, nullptr, GetModuleHandleW(nullptr), this);
  tt_ = mkTooltip(hwnd);
  build();
  return hwnd;
}

LRESULT CALLBACK Page::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Page* p = nullptr;
  if (m == WM_NCCREATE) { p = (Page*)((CREATESTRUCTW*)l)->lpCreateParams; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p); if (p) p->hwnd = h; }
  else p = (Page*)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (p) return p->proc(m, w, l);
  return DefWindowProcW(h, m, w, l);
}

LRESULT Page::proc(UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_COMMAND: {
      auto it = cmds_.find((int)LOWORD(w));
      if (it != cmds_.end()) it->second((int)HIWORD(w));
      return 0;
    }
    case WM_NOTIFY: {
      NMHDR* nh = (NMHDR*)l;
      if (nh && nh->code == LVN_ITEMCHANGED) {
        NMLISTVIEW* lv = (NMLISTVIEW*)l; auto it = noteSel_.find((int)nh->idFrom);
        if (it != noteSel_.end() && (lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED)) it->second(lv->iItem);
      }
      if (nh) onNotify(nh);
      return 0;
    }
    case WM_DRAWITEM: if (w != 0) { drawOwnerButton((const DRAWITEMSTRUCT*)l); return TRUE; } break;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
      HDC dc = (HDC)w; SetBkMode(dc, TRANSPARENT); SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
      return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_HSCROLL: onHScroll((HWND)l); return 0;
    case WM_VSCROLL: {
      SCROLLINFO si; ZeroMemory(&si, sizeof si); si.cbSize = sizeof si; si.fMask = SIF_ALL; GetScrollInfo(hwnd, SB_VERT, &si);
      int pos = si.nPos, line = S(24);
      switch (LOWORD(w)) {
        case SB_LINEUP: pos -= line; break; case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= (int)si.nPage; break; case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break; case SB_BOTTOM: pos = si.nMax; break;
      }
      scrollTo(pos); return 0;
    }
    case WM_MOUSEWHEEL: scrollTo(scroll_ - (short)HIWORD(w) / WHEEL_DELTA * S(48)); return 0;
    case WM_KEYDOWN: if (w == VK_NEXT) { scrollTo(scroll_ + clientH_); return 0; } if (w == VK_PRIOR) { scrollTo(scroll_ - clientH_); return 0; } break;
    default: break;
  }
  onMessage(m, w, l);
  return DefWindowProcW(hwnd, m, w, l);
}

void Page::moveItems() {
  HDWP dw = BeginDeferWindowPos((int)items_.size() + 1);
  for (auto& it : items_) { if (dw) dw = DeferWindowPos(dw, it.h, nullptr, it.x, it.y - scroll_, it.w, it.hgt, SWP_NOZORDER | SWP_NOACTIVATE); }
  if (dw) EndDeferWindowPos(dw);
}

void Page::scrollTo(int pos) {
  RECT rc; GetClientRect(hwnd, &rc); clientH_ = rc.bottom;
  int maxPos = std::max(0, contentH_ - clientH_);
  pos = std::max(0, std::min(pos, maxPos));
  if (pos == scroll_) return;
  scroll_ = pos; moveItems();
  SCROLLINFO si; ZeroMemory(&si, sizeof si); si.cbSize = sizeof si; si.fMask = SIF_POS; si.nPos = scroll_; SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
  RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void Page::place(const RECT& r) {
  SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
  RECT rc; GetClientRect(hwnd, &rc); clientH_ = rc.bottom;
  SCROLLINFO si; ZeroMemory(&si, sizeof si); si.cbSize = sizeof si; si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
  si.nMin = 0; si.nMax = std::max(0, contentH_ - 1); si.nPage = (UINT)clientH_; si.nPos = std::min(scroll_, std::max(0, contentH_ - clientH_));
  SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
  GetClientRect(hwnd, &rc); clientH_ = rc.bottom; contentW_ = rc.right;
  int old = scroll_; scroll_ = std::min(scroll_, std::max(0, contentH_ - clientH_)); if (scroll_ != old) moveItems();
}

void Page::ensureVisible(HWND h) {
  if (!h || !IsChild(hwnd, h)) return;
  RECT rc; GetWindowRect(h, &rc); MapWindowPoints(nullptr, hwnd, (POINT*)&rc, 2);
  RECT cl; GetClientRect(hwnd, &cl);
  if (rc.top < 0) scrollTo(scroll_ + rc.top - S(8));
  else if (rc.bottom > cl.bottom) scrollTo(scroll_ + (rc.bottom - cl.bottom) + S(8));
}

HWND Page::track(HWND h, int x, int y, int w, int hgt, int contentH) {
  if (!h) return h;
  items_.push_back({h, S(x), S(y), S(w), S(hgt)});
  contentH_ = std::max(contentH_, S(y + (contentH >= 0 ? contentH : hgt) + 10));
  SetWindowPos(h, nullptr, S(x), S(y), S(w), S(hgt), SWP_NOZORDER | SWP_NOACTIVATE);
  return h;
}
HWND Page::label(const std::string& t, int x, int y, int w, int h, bool bold) {
  HWND c = mkCtl(hwnd, L"STATIC", t, SS_LEFT | SS_NOPREFIX, S(x), S(y), S(w), S(h), 0);
  if (bold) SendMessageW(c, WM_SETFONT, (WPARAM)g.bold, TRUE);
  return track(c, x, y, w, h);
}
int Page::heading(const std::string& t, int y) {
  HWND c = mkCtl(hwnd, L"STATIC", t, SS_LEFT | SS_NOPREFIX, S(8), S(y), S(760), S(24), 0);
  SendMessageW(c, WM_SETFONT, (WPARAM)g.bold, TRUE); track(c, 8, y, 760, 24);
  HWND sep = mkCtl(hwnd, L"STATIC", "", SS_ETCHEDHORZ, S(8), S(y + 24), S(760), S(2), 0); track(sep, 8, y + 24, 760, 2);
  return y + 32;
}
HWND Page::button(const std::string& t, int x, int y, int w, int h, std::function<void()> cb, const std::string& tipText) {
  int id = newId(); HWND c = mkCtl(hwnd, L"BUTTON", t, BS_PUSHBUTTON | WS_TABSTOP, S(x), S(y), S(w), S(h), id);
  onCmd(id, [cb](int code) { if (code == BN_CLICKED && cb) cb(); });
  tip(tt_, c, tipText.empty() ? t : tipText);
  return track(c, x, y, w, h);
}
HWND Page::obutton(const std::string& t, int x, int y, int w, int h, std::function<void()> cb, const std::string& tipText) {
  int id = newId(); HWND c = mkCtl(hwnd, L"BUTTON", t, BS_OWNERDRAW | WS_TABSTOP, S(x), S(y), S(w), S(h), id);
  onCmd(id, [cb](int code) { if (code == BN_CLICKED && cb) cb(); });
  tip(tt_, c, tipText.empty() ? t : tipText);
  return track(c, x, y, w, h);
}
HWND Page::check(const std::string& t, int x, int y, int w, std::function<void(bool)> cb) {
  int id = newId(); HWND c = mkCtl(hwnd, L"BUTTON", t, BS_AUTOCHECKBOX | WS_TABSTOP, S(x), S(y), S(w), S(24), id);
  onCmd(id, [c, cb](int code) { if (code == BN_CLICKED && cb) cb(SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED); });
  return track(c, x, y, w, 24);
}
HWND Page::radio(const std::string& t, int x, int y, int w, bool first, std::function<void()> cb) {
  int id = newId(); HWND c = mkCtl(hwnd, L"BUTTON", t, BS_AUTORADIOBUTTON | WS_TABSTOP | (first ? WS_GROUP : 0), S(x), S(y), S(w), S(24), id);
  onCmd(id, [cb](int code) { if (code == BN_CLICKED && cb) cb(); });
  return track(c, x, y, w, 24);
}
HWND Page::edit(const std::string& t, int x, int y, int w, int h, DWORD extra, std::function<void()> onChange) {
  int id = newId(); HWND c = mkCtl(hwnd, L"EDIT", t, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | extra, S(x), S(y), S(w), S(h), id);
  if (extra & ES_MULTILINE) { SetWindowLongW(c, GWL_STYLE, GetWindowLongW(c, GWL_STYLE) & ~ES_AUTOHSCROLL); }
  if (onChange) onCmd(id, [onChange](int code) { if (code == EN_CHANGE) onChange(); });
  return track(c, x, y, w, h);
}
HWND Page::combo(int x, int y, int w, std::function<void(int)> onSel, int dropH) {
  int id = newId(); HWND c = mkCtl(hwnd, L"COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, S(x), S(y), S(w), S(dropH), id);
  SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, S(24));
  if (onSel) onCmd(id, [c, onSel](int code) { if (code == CBN_SELCHANGE) onSel((int)SendMessageW(c, CB_GETCURSEL, 0, 0)); });
  return track(c, x, y, w, dropH, 26);   // the window height of a combo includes its drop-down list
}
HWND Page::listbox(int x, int y, int w, int h, std::function<void(int)> onSel) {
  int id = newId(); HWND c = mkCtl(hwnd, L"LISTBOX", "", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP | WS_BORDER, S(x), S(y), S(w), S(h), id);
  if (onSel) onCmd(id, [c, onSel](int code) { if (code == LBN_SELCHANGE) onSel((int)SendMessageW(c, LB_GETCURSEL, 0, 0)); });
  return track(c, x, y, w, h);
}
HWND Page::table(int x, int y, int w, int h, const std::vector<Column>& cols, std::function<void(int)> onSel, bool checks) {
  int id = newId(); std::vector<Column> sc = cols; for (auto& c : sc) c.width = S(c.width);
  HWND c = mkTable(hwnd, S(x), S(y), S(w), S(h), id, sc, checks);
  if (onSel) noteSel_[id] = onSel;
  return track(c, x, y, w, h);
}
HWND Page::trackbar(int x, int y, int w, int h, int lo, int hi, int pos) {
  int id = newId(); HWND c = mkCtl(hwnd, TRACKBAR_CLASSW, "", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, S(x), S(y), S(w), S(h), id);
  SendMessageW(c, TBM_SETRANGE, TRUE, MAKELONG(lo, hi)); SendMessageW(c, TBM_SETPOS, TRUE, pos);
  SendMessageW(c, TBM_SETPAGESIZE, 0, 10); SendMessageW(c, TBM_SETLINESIZE, 0, 1);
  return track(c, x, y, w, h);
}

}} // namespace ss::ui
