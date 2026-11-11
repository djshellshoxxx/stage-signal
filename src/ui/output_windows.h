// Audience output windows (MOD-05 UI half, REQ-D01..D07): borderless popups exactly covering the routed display; no exclusive
// mode, no resolution change, never activated. A window exists only while its route is Active; a lost display destroys it
// (the controls window is untouched) and it returns only when AppController re-binds the SAME identity.
#pragma once
#include "ui/ui_util.h"
#include <map>

namespace ss { namespace ui {
class OutputWindows {
 public:
  void registerClass(HINSTANCE hi);
  void sync();                  // call ~10 Hz from the UI thread: create/move/destroy windows from AppController::outputRoutes()
  void closeAll();
  size_t count() const { return wins_.size(); }
 private:
  struct W { HWND h = nullptr; uint64_t generation = 0; };
  std::map<std::string, W> wins_;
};
}} // namespace ss::ui
