// StageSignal Win32 UI helpers: string conversion, DPI/fonts, palette (system colors; high-contrast aware), control factories,
// owner-drawn state buttons, frame painting (StretchDIBits, aspect fit with black bars), small custom controls.
// Windows-only; lives in src/ui. No exceptions escape window procedures.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <string>
#include <vector>
#include "core/types.h"
#include "core/app/controller.h"

namespace ss { namespace ui {

// ---- strings --------------------------------------------------------------------------------------------------------
std::wstring W(const std::string& utf8);
std::string U8(const std::wstring& w);
std::string fmt(const char* f, ...);
std::string fmtDb(float db);                    // "-23.1 dB" / "-inf dB"
std::string fmtTime(double sec);                // hh:mm:ss

// ---- DPI / fonts / palette --------------------------------------------------------------------------------------------
struct Gfx {
  double scale = 1.0;                           // system DPI / 96 (the manifest declares system DPI awareness)
  HFONT font = nullptr, bold = nullptr, big = nullptr, mono = nullptr;
  int fontPx = 14;                              // effective text height in pixels (>= 14)
  bool highContrast = false;
  COLORREF activeBg, activeFg, alarmBg, alarmFg, okFg;
};
extern Gfx g;
void initGfx();                                 // call once after InitCommonControls
void refreshPalette();                          // on WM_SYSCOLORCHANGE / WM_SETTINGCHANGE
void freeGfx();
inline int S(int v) { return (int)(v * g.scale + (v >= 0 ? 0.5 : -0.5)); }
int effW(int px); int effH(int px);             // pixels -> effective 96-dpi units

// ---- control factory --------------------------------------------------------------------------------------------------
HWND mkCtl(HWND parent, const wchar_t* cls, const std::string& text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0);
void setTextIf(HWND h, const std::string& utf8);        // avoids flicker: only sets when different
std::string getText(HWND h);
void setEnabled(HWND h, bool on);
void tip(HWND tt, HWND target, const std::string& text);
HWND mkTooltip(HWND parent);
std::string className(HWND h);
bool isTextField(HWND h);                       // Edit / RichEdit / editable combo: digits must not trigger scene pads

// ---- combo / list helpers ------------------------------------------------------------------------------------------------
void comboFill(HWND cb, const std::vector<std::string>& items, int sel);
int comboSel(HWND cb);
void listFill(HWND lb, const std::vector<std::string>& items, int sel);

// ---- owner-drawn state buttons (pads, strip, mode) ---------------------------------------------------------------------------
enum BtnState { BS_NORMAL_ = 0, BS_ACTIVE_ = 1, BS_ALARM_ = 2 };
void setBtnState(HWND b, BtnState s);
void drawOwnerButton(const DRAWITEMSTRUCT* d);

// ---- ListView table ------------------------------------------------------------------------------------------------------------
struct Column { std::string title; int width; };
HWND mkTable(HWND parent, int x, int y, int w, int h, int id, const std::vector<Column>& cols, bool checkboxes = false);
void tableSet(HWND lv, const std::vector<std::vector<std::string>>& rows);   // rebuilds only if content changed; keeps selection
int tableSel(HWND lv);

// ---- frames -----------------------------------------------------------------------------------------------------------------------
// Aspect-fit a frame into rc (black bars); StretchDIBits with COLORONCOLOR. f may be null (black).
void paintFrame(HDC dc, const RECT& rc, const Frame* f);
bool frameDiffers(const app::FramePtr& a, const app::FramePtr& b);

// ---- custom controls --------------------------------------------------------------------------------------------------------------
void registerControlClasses(HINSTANCE hi);      // "SSFrameView", "SSMeter"
struct FrameViewData;                           // window-held state
HWND mkFrameView(HWND parent, int x, int y, int w, int h);
void frameViewSet(HWND v, const app::FramePtr& f);
HWND mkMeter(HWND parent, int x, int y, int w, int h);
void meterSet(HWND m, const float rms[2], const float peak[2], bool nearFull, bool clip);
void drawMeterBars(HDC dc, const RECT& rc, const float rms[2], const float peak[2]);

// ---- files / dialogs ----------------------------------------------------------------------------------------------------------------
bool openFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter, std::wstring& out, bool multi = false, std::vector<std::wstring>* many = nullptr);
bool saveFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter, const wchar_t* defExt, std::wstring& out);
bool pickFolder(HWND owner, const wchar_t* title, std::wstring& out);
int msgBox(HWND owner, const std::string& text, const std::string& caption, UINT flags);

// ---- global UI context (single UI thread) ----------------------------------------------------------------------------------------
class MainWindow;
struct UiApp {
  app::AppController* ctl = nullptr;
  HINSTANCE inst = nullptr;
  HWND mainWnd = nullptr;
  MainWindow* main = nullptr;
  std::string dataDir, showPath;                // showPath: current show file ("" = unsaved)
  std::string version = "0.1.0";
  const EncoderRegistry* encoders = nullptr;                          // for honest codec availability text
  std::function<std::string(BroadcastProtocol)> protocolIssue;        // "" = available, else the reason it is not
};
extern UiApp A;

// Posts a message for the operator message bar from any thread (e.g. action acknowledgements).
void postNotice(const std::string& text, bool isError);
// Submits an action; rejection reasons are shown in the message bar.
void act(ActionId id, double value = 0, int target = -1, ActionSource src = ActionSource::UI);
constexpr UINT WM_SS_NOTICE = WM_APP + 41;
constexpr UINT WM_SS_KEY = WM_APP + 42;         // key forwarded from an audience window (never focused) to the dispatcher
std::string takeNotice(bool* isError);

// Waits for a future with a bounded timeout on the UI thread.
Status waitStatus(std::future<Status>& f, int ms = 4000);

}} // namespace ss::ui
