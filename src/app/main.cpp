// StageSignal.exe entry point: single instance GUI, plus headless --version / --selftest / --benchmark / --uitest.
// Offline by design: no network, no login. DMX, recording and broadcast always start disarmed.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <cstdio>
#include <fstream>
#include <thread>
#include "app/version.h"
#include "app/real_deps.h"
#include "ui/ui_util.h"
#include "ui/main_window.h"
#include "ui/pages.h"

using namespace ss;

namespace {
void outText(const std::string& s) {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!h || h == INVALID_HANDLE_VALUE) { if (AttachConsole(ATTACH_PARENT_PROCESS)) h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr); }
  if (h && h != INVALID_HANDLE_VALUE) { DWORD n = 0; WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr); }
}
std::string archName() { return sizeof(void*) == 8 ? "x64" : "x86-base"; }
std::string versionLine() { return std::string("StageSignal ") + SS_VERSION + " (" + archName() + ", Visual Core Beta)\n"; }

std::string localDataDir() {
  wchar_t p[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathW(nullptr, 0x001c /*CSIDL_LOCAL_APPDATA*/, nullptr, 0, p)) && p[0]) return ui::U8(p) + "\\StageSignal";
  wchar_t e[MAX_PATH] = {}; GetModuleFileNameW(nullptr, e, MAX_PATH); std::string s = ui::U8(e); size_t k = s.find_last_of("\\/"); return (k == std::string::npos ? std::string(".") : s.substr(0, k)) + "\\data";
}
void mkdirs(const std::string& d) { std::wstring w = ui::W(d); for (size_t i = 3; i < w.size(); ++i) if (w[i] == L'\\' || w[i] == L'/') { std::wstring part = w.substr(0, i); CreateDirectoryW(part.c_str(), nullptr); } CreateDirectoryW(w.c_str(), nullptr); }

void writeFile(const std::string& path, const std::string& data) {
  HANDLE f = CreateFileW(ui::W(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) { DWORD n = 0; WriteFile(f, data.data(), (DWORD)data.size(), &n, nullptr); CloseHandle(f); }
}

struct Args { bool version = false, selftest = false, benchmark = false, uitest = false; std::string out, dataDir; double scale = 1.0; };
Args parseArgs() {
  Args a; int n = 0; LPWSTR* v = CommandLineToArgvW(GetCommandLineW(), &n); if (!v) return a;
  for (int i = 1; i < n; ++i) {
    std::string s = ui::U8(v[i]);
    if (s == "--version" || s == "-v") a.version = true; else if (s == "--selftest") a.selftest = true; else if (s == "--benchmark") a.benchmark = true; else if (s == "--uitest") { a.uitest = true; if (i + 1 < n) a.out = ui::U8(v[++i]); }
    else if (s == "--out" && i + 1 < n) a.out = ui::U8(v[++i]); else if (s == "--data-dir" && i + 1 < n) a.dataDir = ui::U8(v[++i]); else if (s == "--scale" && i + 1 < n) a.scale = atof(ui::U8(v[++i]).c_str());
  }
  LocalFree(v); return a;
}
std::string tempDir(const char* tag) { wchar_t t[MAX_PATH] = {}; GetTempPathW(MAX_PATH, t); return ui::U8(t) + "stagesignal-" + tag + "-" + std::to_string(GetCurrentProcessId()); }

app::AppConfig makeConfig(const std::string& dataDir) { app::AppConfig c; c.dataDir = dataDir; c.mediaRoot = dataDir + "/media-root"; return c; }

// Headless smoke: controller with no capture devices (real display/text/disk adapters), 2 s of rendering, ordered shutdown.
int runSelfTest(const Args& a) {
  std::string dd = a.dataDir.empty() ? tempDir("selftest") : a.dataDir; mkdirs(dd);
  int rc = 1; std::string why;
  {
    app::RealDepsOptions ro; ro.devices = false; app::RealDeps rd(ro);
    app::AppController ctl(makeConfig(dd), rd.deps());
    Status s = ctl.start(); if (!s.ok()) { why = "start failed: " + s.detail; }
    else {
      ActionResult dummy; Action n; n.id = ActionId::NextScene; ctl.submit(n).wait_for(std::chrono::seconds(2));
      std::this_thread::sleep_for(std::chrono::milliseconds(2000));
      app::AppStatus st = ctl.status(); app::FramePtr f; bool pv = ctl.previewFrame(f);
      bool armedAnything = st.dmxArmed || st.recState != RecState::Idle || st.liveState != BroadcastState::Off;
      if (st.framesRendered == 0) why = "no frames rendered"; else if (!pv || !f || f->w <= 0) why = "no preview frame"; else if (armedAnything) why = "something armed at start";
      else rc = 0;
      outText(ui::fmt("selftest: frames=%llu preview=%dx%d routes=%zu dmxArmed=%d rec=%s live=%s\n", (unsigned long long)st.framesRendered, f ? f->w : 0, f ? f->h : 0, st.outputs.size(), (int)st.dmxArmed, recStateName(st.recState), st.liveText.c_str()));
      app::ShutdownReport rep = ctl.shutdown(); if (!rep.clean) { rc = 1; why = "shutdown not clean"; for (auto& g2 : rep.stages) if (!g2.ok) why += " (" + g2.name + ": " + g2.detail + ")"; }
    }
  }
  outText(rc == 0 ? "SELFTEST OK\n" : "SELFTEST FAILED: " + why + "\n");
  return rc;
}

int runBenchmark(const Args& a) {
  std::string dd = a.dataDir.empty() ? tempDir("bench") : a.dataDir; mkdirs(dd);
  app::RealDepsOptions ro; ro.devices = false; app::RealDeps rd(ro);
  app::AppController ctl(makeConfig(dd), rd.deps()); Status s = ctl.start(); if (!s.ok()) { outText("benchmark: start failed: " + s.detail + "\n"); return 1; }
  app::BenchmarkRequest rq; rq.durationScale = a.scale > 0 ? a.scale : 1.0;
  auto f = ctl.runBenchmark(rq); app::BenchmarkOutcome o = f.get();
  if (!o.status.ok()) { outText("benchmark failed: " + o.status.detail + "\n"); ctl.shutdown(); return 1; }
  std::string json = o.result.toJson(2); std::string path = a.out.empty() ? dd + "\\benchmark.json" : a.out;
  writeFile(path, json);
  outText("benchmark: tier=" + o.profile.tier + (o.result.qualifying ? " qualifying" : " NON-QUALIFYING (scaled/partial)") + std::string(" gates=") + (o.profile.gatesPassed ? "pass" : "fail") + " json=" + path + "\n");
  ctl.shutdown(); return o.profile.gatesPassed ? 0 : 3;
}

void firstRunHelp(const std::string& dd) {
  std::string marker = dd + "\\first-run-done"; DWORD at = GetFileAttributesW(ui::W(marker).c_str());
  if (at == INVALID_FILE_ATTRIBUTES) { ui::showHelp(0); writeFile(marker, "1"); }
}

void recoveryPrompt(app::AppController& ctl, HWND owner, const std::string& dd) {
  persist::RecoveryInfo ri = ctl.recoveryInfo(); if (!ri.crashed) return;
  if (!ri.autosaveValid) { ui::msgBox(owner, "StageSignal did not close cleanly last time, and no valid autosave was found. Starting with an empty show.", "Crash recovery", MB_OK | MB_ICONINFORMATION); ctl.discardRecovery(); return; }
  std::string msg = "StageSignal did not close cleanly last time.\n\nAn autosaved copy of \"" + ri.showName + "\" is available.\nProposed restore: load it as a NEW COPY (your original file is never overwritten). The restored show starts idle with DMX, recording and broadcast DISARMED; you re-arm them yourself.\n\nRestore the autosaved copy?";
  if (ui::msgBox(owner, msg, "Crash recovery", MB_YESNO | MB_ICONQUESTION) == IDYES) {
    std::string path = dd + "\\recovered-" + std::to_string(GetTickCount()) + ".stagesignal";
    auto f = ctl.restoreRecovery(path); Status s = ui::waitStatus(f, 15000);
    if (s.ok()) { ui::A.showPath = path; ui::postNotice("Restored copy loaded; everything is disarmed.", false); }
    else ui::msgBox(owner, "The autosave could not be restored: " + s.detail, "Crash recovery", MB_OK | MB_ICONERROR);
  } else ctl.discardRecovery();
}

int runGui(HINSTANCE hi, int nShow, const Args& a) {
  bool uitest = a.uitest;
  HANDLE mutex = nullptr;
  if (!uitest) {
    mutex = CreateMutexW(nullptr, TRUE, L"Local\\StageSignal.SingleInstance.v1");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
      HWND w = FindWindowW(L"StageSignalMain", nullptr); if (w) { if (IsIconic(w)) ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
      CloseHandle(mutex); return 0;
    }
  }
  INITCOMMONCONTROLSEX ic; ic.dwSize = sizeof ic; ic.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES; InitCommonControlsEx(&ic);
  OleInitialize(nullptr); ui::initGfx();
  std::string dd = !a.dataDir.empty() ? a.dataDir : (uitest ? tempDir("ui") : localDataDir()); mkdirs(dd);
  int code = 0;
  {
    app::RealDepsOptions ro; ro.devices = !uitest; app::RealDeps rd(ro);
    app::AppController ctl(makeConfig(dd), rd.deps());
    Status s = ctl.start();
    ui::A.ctl = &ctl; ui::A.inst = hi; ui::A.dataDir = dd; ui::A.encoders = &rd.encoders(); ui::A.protocolIssue = [&rd](BroadcastProtocol p) { return rd.protocolIssue(p); };
    if (!s.ok()) ui::msgBox(nullptr, "StageSignal started with a problem: " + s.detail, "StageSignal", MB_OK | MB_ICONWARNING);
    ui::MainWindow mw;
    if (!mw.create(hi, nShow)) { ui::msgBox(nullptr, "The control window could not be created.", "StageSignal", MB_OK | MB_ICONERROR); code = 1; }
    else if (uitest) {
      mkdirs(a.out); DWORD t0 = GetTickCount();
      for (int tab = 0; tab < mw.tabCount(); ++tab) {
        mw.showTab(tab); DWORD t1 = GetTickCount(); while (GetTickCount() - t1 < 700) { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } mw.tick(); Sleep(30); }
        mw.screenshot(ui::W(a.out + "\\tab" + std::to_string(tab) + ".bmp"));
      }
      (void)t0; DestroyWindow(mw.hwnd()); MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
      outText("UITEST OK: " + std::to_string(mw.tabCount()) + " tabs rendered to " + a.out + "\n");
    } else {
      recoveryPrompt(ctl, mw.hwnd(), dd); firstRunHelp(dd); ui::runMessageLoop(mw);
    }
    app::ShutdownReport rep = ctl.shutdown(); (void)rep;
    ui::A.ctl = nullptr;
  }
  ui::freeGfx(); OleUninitialize(); if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
  return code;
}
} // namespace

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int nShow) {
  Args a = parseArgs();
  if (a.version) { outText(versionLine()); return 0; }
  if (a.selftest) return runSelfTest(a);
  if (a.benchmark) return runBenchmark(a);
  return runGui(hi, nShow, a);
}
