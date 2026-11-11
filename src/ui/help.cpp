// Offline help viewer and About window. Topic text lives in docs/help/*.md and licenses/*.md, embedded as RCDATA resources
// (HELP_nn_*, LICENSE_*), so help works with no network and no installed files. Rich text via RichEdit when present.
#include "ui/pages.h"
#include "app/version.h"
#include <richedit.h>
#include <algorithm>

namespace ss { namespace ui {
namespace {
struct Topic { std::string title, body; };

std::string loadRes(const wchar_t* name) {
  HRSRC r = FindResourceW(nullptr, name, RT_RCDATA); if (!r) return std::string();
  HGLOBAL g2 = LoadResource(nullptr, r); DWORD n = SizeofResource(nullptr, r); const char* p = (const char*)LockResource(g2);
  return (p && n) ? std::string(p, n) : std::string();
}
BOOL CALLBACK enumNames(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR lp) {
  if (!IS_INTRESOURCE(name) && wcsncmp(name, L"HELP_", 5) == 0) ((std::vector<std::wstring>*)lp)->push_back(name);
  return TRUE;
}
std::vector<Topic> helpTopics() {
  std::vector<std::wstring> names; EnumResourceNamesW(nullptr, RT_RCDATA, enumNames, (LONG_PTR)&names); std::sort(names.begin(), names.end());
  std::vector<Topic> v;
  for (auto& n : names) { Topic t; t.body = loadRes(n.c_str()); size_t e = t.body.find('\n'); std::string first = t.body.substr(0, e);
    while (!first.empty() && (first[0] == '#' || first[0] == ' ')) first.erase(0, 1); while (!first.empty() && (first.back() == '\r')) first.pop_back(); t.title = first.empty() ? U8(n) : first; v.push_back(t); }
  if (v.empty()) v.push_back({"Help", "Help text is not embedded in this build."});
  return v;
}

class TextWindow {
 public:
  TextWindow(const std::string& title, std::vector<Topic> t) : title_(title), topics_(std::move(t)) {}
  void show(int topic, HWND owner) {
    if (!hw_) create(owner);
    if (topic >= 0 && topic < (int)topics_.size()) { SendMessageW(list_, LB_SETCURSEL, (WPARAM)topic, 0); select(topic); }
    ShowWindow(hw_, SW_SHOW); SetForegroundWindow(hw_); SetFocus(list_);
  }
  int indexOf(const std::string& key) const { for (size_t i = 0; i < topics_.size(); ++i) if (topics_[i].title.find(key) != std::string::npos) return (int)i; return 0; }
 private:
  void create(HWND owner) {
    static bool reg = false;
    if (!reg) { WNDCLASSW wc; ZeroMemory(&wc, sizeof wc); wc.lpfnWndProc = &TextWindow::Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"SSTextWindow"; wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1)); RegisterClassW(&wc); reg = true; }
    LoadLibraryW(L"Msftedit.dll");
    hw_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"SSTextWindow", W(title_).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(760), S(520), owner, nullptr, GetModuleHandleW(nullptr), this);
    list_ = mkCtl(hw_, L"LISTBOX", "", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, 0, 0, 100, 100, 1001, WS_EX_CLIENTEDGE);
    rich_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 100, 100, hw_, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);
    isRich_ = rich_ != nullptr;
    if (!rich_) rich_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 100, 100, hw_, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(rich_, WM_SETFONT, (WPARAM)g.font, TRUE);
    for (auto& t : topics_) SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)W(t.title).c_str());
    RECT rc; GetClientRect(hw_, &rc); layout(rc.right, rc.bottom); SendMessageW(list_, LB_SETCURSEL, 0, 0); select(0);
  }
  void layout(int w, int h) { int lw = S(230); MoveWindow(list_, 0, 0, lw, h, TRUE); MoveWindow(rich_, lw + 2, 0, w - lw - 2, h, TRUE); }
  void select(int i) {
    if (i < 0 || i >= (int)topics_.size()) return;
    std::vector<std::string> lines; std::string cur; for (char c : topics_[(size_t)i].body) { if (c == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); lines.push_back(cur); cur.clear(); } else cur += c; } if (!cur.empty()) lines.push_back(cur);
    std::wstring all; std::vector<std::pair<int, int>> heads; int pos = 0; const wchar_t* sep = isRich_ ? L"\r" : L"\r\n";
    for (auto& l : lines) { std::string t = l; bool head = !t.empty() && t[0] == '#'; if (head) { while (!t.empty() && (t[0] == '#' || t[0] == ' ')) t.erase(0, 1); }
      std::wstring w = W(t); if (head) heads.push_back({pos, pos + (int)w.size()}); all += w; all += sep; pos += (int)w.size() + (int)wcslen(sep); }
    SetWindowTextW(rich_, all.c_str());
    if (isRich_) for (auto& hd : heads) { SendMessageW(rich_, EM_SETSEL, hd.first, hd.second); CHARFORMAT2W cf; ZeroMemory(&cf, sizeof cf); cf.cbSize = sizeof cf; cf.dwMask = CFM_BOLD | CFM_SIZE; cf.dwEffects = CFE_BOLD; cf.yHeight = 20 * (g.fontPx * 3 / 2) * 72 / 96; SendMessageW(rich_, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf); }
    if (isRich_) SendMessageW(rich_, EM_SETSEL, 0, 0);
  }
  static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    TextWindow* t = (TextWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) { t = (TextWindow*)((CREATESTRUCTW*)l)->lpCreateParams; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)t); t->hw_ = h; }
    if (t) switch (m) {
      case WM_SIZE: if (t->list_) t->layout(LOWORD(l), HIWORD(l)); return 0;
      case WM_COMMAND: if (LOWORD(w) == 1001 && HIWORD(w) == LBN_SELCHANGE) t->select((int)SendMessageW(t->list_, LB_GETCURSEL, 0, 0)); if (LOWORD(w) == IDCANCEL) ShowWindow(h, SW_HIDE); return 0;
      case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    }
    return DefWindowProcW(h, m, w, l);
  }
  std::string title_; std::vector<Topic> topics_; HWND hw_ = nullptr, list_ = nullptr, rich_ = nullptr; bool isRich_ = false;
};
TextWindow* gHelp = nullptr; TextWindow* gAbout = nullptr;
} // namespace

void showHelp(int topic) { if (!gHelp) gHelp = new TextWindow("StageSignal Help (offline)", helpTopics()); gHelp->show(topic, nullptr); }
void showHelpTopic(const std::string& key) { if (!gHelp) gHelp = new TextWindow("StageSignal Help (offline)", helpTopics()); gHelp->show(gHelp->indexOf(key), nullptr); }
void showAbout(HWND owner) {
  if (!gAbout) {
    std::vector<Topic> t;
    std::string about = std::string("# About StageSignal\nStageSignal ") + SS_VERSION + " - Visual Core Beta (unsigned build)\n\nAudio-reactive visuals, programmed cues, recording and optional broadcast for live shows. Runs fully offline: no login, no network access unless you start a broadcast.\n\nBuild: " +
      (sizeof(void*) == 8 ? "64-bit" : "32-bit (BASE x86, i586, no SSE)") + ", " + __DATE__ + "\n\nStageSignal does not install drivers for USB DMX adapters. No adapter is listed as hardware-qualified until recorded test results exist.\n\nThe H.264 encoder uses the Windows Media Foundation component present on your system; patent and redistribution decisions for H.264 are separate from this software.\n";
    t.push_back({"About StageSignal", about});
    t.push_back({"Licenses overview", loadRes(L"LICENSE_README")});
    t.push_back({"Third-party notices", loadRes(L"LICENSE_THIRD_PARTY")});
    gAbout = new TextWindow("About StageSignal", t);
  }
  gAbout->show(0, owner);
}
}} // namespace ss::ui
