// Control window (spec §11): persistent 48 px status bar, persistent 60 px performance strip, tab pages, keyboard dispatcher.
#pragma once
#include "ui/ui_util.h"
#include "ui/ui_page.h"
#include "ui/output_windows.h"
#include <memory>

namespace ss { namespace ui {

class MirrorWindow;

class MainWindow {
 public:
  bool create(HINSTANCE hi, int nCmdShow);
  HWND hwnd() const { return hwnd_; }
  bool preTranslate(MSG& m);                    // call from the message loop before TranslateMessage; true = consumed
  void tick();                                  // 10 Hz poll (also callable from the smoke test)
  void showTab(int i);
  bool handleKey(WPARAM vk, LPARAM lp);         // shortcuts; false = not ours
  bool doSave(bool saveAs);
  bool confirmDiscard();                        // true = ok to proceed (saved/discarded)
  void screenshot(const std::wstring& pngBmpPath);   // --uitest: BMP of the client area via PrintWindow/BitBlt
  int tabCount() const { return (int)pages_.size(); }
  bool compact() const { return compact_; }
  static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
 private:
  LRESULT proc(UINT, WPARAM, LPARAM);
  void layout();
  void onCommand(int id, int code, HWND src);
  void buildStrip();
  void updateStrip(const app::AppStatus& st, const show::ShowSnapshot& sn);
  void updateTopBar(const app::AppStatus& st);
  void doOpen(); void doNew(); void doExport(); void doImport();
  HWND hwnd_ = nullptr, top_ = nullptr, tab_ = nullptr, msg_ = nullptr, strip_ = nullptr, tt_ = nullptr;
  HWND bVis_ = nullptr, bLight_ = nullptr, bEmer_ = nullptr, bRestore_ = nullptr, bPause_ = nullptr, bResume_ = nullptr, bNext_ = nullptr, tbBri_ = nullptr, tbInt_ = nullptr, lBri_ = nullptr, lInt_ = nullptr;
  std::vector<std::unique_ptr<Page>> pages_; int cur_ = 0;
  OutputWindows outs_; bool compact_ = false; DWORD noticeAt_ = 0; std::string title_; int cw_ = 0, ch_ = 0;
  std::unique_ptr<MirrorWindow> mirror_;
};

// Optional third control window mirroring state; actions carry the revision they were based on, a mismatch shows a conflict banner.
class MirrorWindow {
 public:
  void show(HWND owner);
  void tick();
  HWND hwnd = nullptr;
 private:
  static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
  void send(ActionId id);
  HWND lbl_ = nullptr, banner_ = nullptr, bNext_ = nullptr, bPrev_ = nullptr, bBlack_ = nullptr, bRestore_ = nullptr;
  uint64_t baseRev_ = 0; bool haveBase_ = false; std::string note_; HWND bAccept_ = nullptr;
};

int runMessageLoop(MainWindow& w);
}} // namespace ss::ui
