// Diagnostics tab: event journal, adaptation step, metrics, queue depths, warnings and support-bundle export with a
// category preview (the operator approves categories; secrets and paths are redacted by the diagnostics module).
#include "ui/pages.h"
#include "diagnostics/event_journal.h"
#include <set>

namespace ss { namespace ui {
namespace {
class DiagPage : public Page {
 public:
  DiagPage() : Page("Diagnostics") {}
  void build() override {
    int y = 4;
    y = heading("Status, adaptation and queues", y);
    lblAdapt_ = label("", 8, y, 760, 64); y += 68;
    chkLock_ = check("Lock quality (overload shows a warning instead of changing the profile)", 8, y, 560, [](bool on) { A.ctl->setQualityLocked(on); });
    button("Add marker", 580, y - 3, 110, 32, [] { A.ctl->addMarker("Operator marker"); postNotice("Marker added to the journal.", false); }); y += 34;
    lblMetrics_ = label("", 8, y, 760, 150); y += 154;
    y = heading("Warnings", y);
    lblWarn_ = label("", 8, y, 760, 60); y += 64;
    y = heading("Event journal (newest first; monotonic and wall-clock times)", y);
    tblJ_ = table(8, y, 760, 170, {{"#", 50}, {"Wall time", 130}, {"Mono s", 70}, {"Kind", 80}, {"Subject", 150}, {"Detail", 270}}); y += 178;
    y = heading("Support bundle (sanitized; you approve every category)", y);
    button("Preview categories", 8, y, 150, 32, [this] { preview(); });
    button("Export bundle...", 164, y, 150, 32, [this] { exportBundle(); });
    lblB_ = label("Nothing is sent anywhere: the bundle is a ZIP file you choose to save and share yourself.", 322, y + 6, 446, 40); y += 40;
    tblB_ = table(8, y, 760, 120, {{"Category", 140}, {"What is included", 400}, {"Files", 60}, {"Bytes", 100}}, nullptr, true); y += 128;
    setContentHeight(y);
  }
  void refresh() override {
    auto st = ctl().status();
    setTextIf(lblAdapt_, fmt("Adaptation: level %d, step '%s'%s | scene %.0f fps, preview %.0f fps, render scale %.2f\r\nFrames rendered %llu, deadlines missed %llu | capture overruns %llu\r\nAction queue %zu (rejected full: %llu), capture queue %zu (drops %llu), cast audio queue %zu (drops %llu)",
      st.adaptationLevel, st.adaptationStep.c_str(), st.qualityLocked ? " [quality LOCKED]" : "", st.sceneFps, st.previewFps, st.renderScale, (unsigned long long)st.framesRendered, (unsigned long long)st.deadlinesMissed, (unsigned long long)st.captureOverruns,
      st.actionQueueDepth, (unsigned long long)st.actionsRejectedFull, st.captureQueueDepth, (unsigned long long)st.captureDrops, st.castAudioQueueDepth, (unsigned long long)st.castAudioDrops));
    std::string w; for (auto& x : st.warnings) w += (x.critical ? "[CRITICAL] " : "[warning] ") + x.text + "\r\n"; setTextIf(lblWarn_, w.empty() ? "No warnings." : w);
    if (++tick_ % 10 == 1) {
      MetricsSnapshot m = ctl().metrics(); std::string t = fmt("Presented %llu frames (%.1f fps of %.0f target), stalls %llu, missed deadlines %.1f%%\r\n", (unsigned long long)m.framesPresented, m.presentFps, m.targetFps, (unsigned long long)m.presentStalls, m.missedFraction * 100);
      t += m.renderTimeMs.available ? fmt("Render time ms p50 %.1f p95 %.1f max %.1f\r\n", m.renderTimeMs.p50, m.renderTimeMs.p95, m.renderTimeMs.max) : "Render time: unavailable\r\n";
      t += m.cpu.available ? fmt("CPU process avg %.1f%% p95 %.1f%%, system avg %.1f%%\r\n", m.cpu.procAvg, m.cpu.procP95, m.cpu.sysAvg) : "CPU counters: unavailable\r\n";
      t += m.mem.wsAvailable ? fmt("Memory working set %.1f MB (peak %.1f MB)\r\n", m.mem.wsSteady / 1048576.0, m.mem.wsPeak / 1048576.0) : "Memory counters: unavailable\r\n";
      t += fmt("Drops: render %llu, present %llu, decode %llu, encode %llu\r\n", (unsigned long long)m.drops[0], (unsigned long long)m.drops[1], (unsigned long long)m.drops[2], (unsigned long long)m.drops[3]);
      for (auto& q : m.queues) t += fmt("Queue %s: %zu/%zu (max %.0f%%)  ", q.name.c_str(), q.current, q.capacity, q.maxFraction * 100);
      setTextIf(lblMetrics_, t);
      auto ev = ctl().journalSnapshot(); std::vector<std::vector<std::string>> rows; size_t n = 0;
      for (size_t i = ev.size(); i-- > 0 && n < 200; ++n) { auto& e = ev[i]; time_t tt = (time_t)(e.wallMs / 1000); struct tm* tmv = gmtime(&tt); char b[32] = ""; if (tmv) strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", tmv);
        rows.push_back({std::to_string(e.seq), b, fmt("%.2f", nsToSec(e.monoNs)), journalKindName(e.kind), e.subject, e.detail}); }
      tableSet(tblJ_, rows);
    }
  }
 private:
  void preview() {
    cats_ = ctl().supportBundlePreview(); std::vector<std::vector<std::string>> rows;
    for (auto& c : cats_) rows.push_back({c.title, c.description, std::to_string(c.files), std::to_string(c.approxBytes)});
    tableSet(tblB_, rows);
    for (size_t i = 0; i < cats_.size(); ++i) { LVITEMW it; ZeroMemory(&it, sizeof it); it.stateMask = LVIS_STATEIMAGEMASK; it.state = INDEXTOSTATEIMAGEMASK(2); SendMessageW(tblB_, LVM_SETITEMSTATE, i, (LPARAM)&it); }
    setTextIf(lblB_, "Untick any category you do not want to share, then Export.");
  }
  void exportBundle() {
    if (cats_.empty()) { postNotice("Preview the categories first.", true); return; }
    std::set<std::string> ok; for (size_t i = 0; i < cats_.size(); ++i) if (ListView_GetCheckState(tblB_, (int)i)) ok.insert(cats_[i].id);
    if (ok.empty()) { postNotice("No category approved: nothing exported.", true); return; }
    std::wstring p = L"stagesignal-support.zip"; if (!saveFileDialog(A.mainWnd, L"Export support bundle", L"ZIP archive\0*.zip\0", L"zip", p)) return;
    BundleReport rep; Status s = ctl().exportSupportBundle(U8(p), ok, &rep);
    postNotice(s.ok() ? fmt("Support bundle written: %zu file(s), %llu bytes.", rep.files, (unsigned long long)rep.bytes) : "Export failed: " + s.detail, !s.ok());
  }
  HWND lblAdapt_ = nullptr, chkLock_ = nullptr, lblMetrics_ = nullptr, lblWarn_ = nullptr, tblJ_ = nullptr, lblB_ = nullptr, tblB_ = nullptr;
  std::vector<BundleCategory> cats_; int tick_ = 0;
};
} // namespace
std::unique_ptr<Page> makeDiagPage() { return std::unique_ptr<Page>(new DiagPage()); }
}} // namespace ss::ui
