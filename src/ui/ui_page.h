// Page: a scrollable tab page (child window of the main window). Coordinates passed to the helpers are logical 96-dpi pixels
// (scaled by ui::S). Controls are tracked so vertical scrolling and "scroll the focused control into view" work for
// keyboard-only operation (REQ-D10).
#pragma once
#include "ui/ui_util.h"
#include <map>
#include <memory>

namespace ss { namespace ui {

class Page {
 public:
  explicit Page(std::string t) : title(std::move(t)) {}
  virtual ~Page() = default;
  HWND create(HWND parent);                   // registers the class once, creates the window, calls build()
  void place(const RECT& r);                  // position inside the parent and (re)configure the vertical scrollbar
  void ensureVisible(HWND h);
  bool owns(HWND h) const { return h && (h == hwnd || IsChild(hwnd, h)); }
  virtual void refresh() {}                   // ~10 Hz while the page is visible
  virtual void activated() {}                 // page became visible
  virtual void onNotify(NMHDR*) {}
  virtual void onHScroll(HWND) {}
  virtual void onMessage(UINT, WPARAM, LPARAM) {}
  HWND hwnd = nullptr; std::string title;
  int contentW() const { return contentW_; }

 protected:
  virtual void build() = 0;
  app::AppController& ctl() { return *A.ctl; }
  int newId() { return nextId_++; }
  void onCmd(int id, std::function<void(int)> f) { cmds_[id] = std::move(f); }
  HWND track(HWND h, int x, int y, int w, int hgt, int contentH = -1);
  HWND label(const std::string& t, int x, int y, int w, int h = 20, bool bold = false);
  int heading(const std::string& t, int y);   // bold caption + separator; returns next y
  HWND button(const std::string& t, int x, int y, int w, int h, std::function<void()> cb, const std::string& tipText = std::string());
  HWND obutton(const std::string& t, int x, int y, int w, int h, std::function<void()> cb, const std::string& tipText = std::string());  // owner-drawn state button
  HWND check(const std::string& t, int x, int y, int w, std::function<void(bool)> cb);
  HWND radio(const std::string& t, int x, int y, int w, bool first, std::function<void()> cb);
  HWND edit(const std::string& t, int x, int y, int w, int h = 26, DWORD extra = 0, std::function<void()> onChange = nullptr);
  HWND combo(int x, int y, int w, std::function<void(int)> onSel = nullptr, int dropH = 200);
  HWND listbox(int x, int y, int w, int h, std::function<void(int)> onSel = nullptr);
  HWND table(int x, int y, int w, int h, const std::vector<Column>& cols, std::function<void(int)> onSel = nullptr, bool checks = false);
  HWND trackbar(int x, int y, int w, int h, int lo, int hi, int pos);
  void setContentHeight(int bottomLogical) { contentH_ = std::max(contentH_, S(bottomLogical)); }
  void tipFor(HWND h, const std::string& text) { tip(tt_, h, text); }
  bool visible() const { return hwnd && IsWindowVisible(hwnd); }

 private:
  struct Item { HWND h; int x, y, w, hgt; };
  static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT proc(UINT, WPARAM, LPARAM);
  void moveItems();
  void scrollTo(int pos);
  std::vector<Item> items_;
  std::map<int, std::function<void(int)>> cmds_;
  std::map<int, std::function<void(int)>> noteSel_;
  HWND tt_ = nullptr;
  int nextId_ = 100, contentH_ = 0, contentW_ = 0, scroll_ = 0, clientH_ = 0;
};

}} // namespace ss::ui
