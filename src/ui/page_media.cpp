// Media/Text tab: validated import (stills/GIF/video), missing-media list, scene and cue editor basics, text pipeline
// (Draft -> Preview -> Publish, Unicode edit box, per-output target) and per-output overlay switches (REQ-A09).
#include "ui/pages.h"
#include "media/import.h"
#include "media/sha256.h"
#include "platform/windows/platform.h"
#include <algorithm>
#include <set>

namespace ss { namespace ui {
namespace {
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string extOf(const std::string& p) { size_t d = p.find_last_of('.'); return d == std::string::npos ? "" : lower(p.substr(d + 1)); }

class MediaPage : public Page {
 public:
  MediaPage() : Page("Media / Text") {}
  void build() override {
    int y = 4;
    y = heading("Media import (PNG, JPEG, BMP, GIF; MP4/MOV/MKV where the video decoder exists)", y);
    button("Import media...", 8, y, 140, 32, [this] { importMedia(); });
    label("Files are validated (type, signature, size, dimensions) before they are copied into the show's media folder.", 156, y + 6, 612, 22);
    y += 38;
    edLog_ = edit("", 8, y, 760, 70, ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL); y += 76;
    tblMedia_ = table(8, y, 760, 100, {{"File", 300}, {"Format", 70}, {"Size", 100}, {"Status", 240}}); y += 106;
    lblMissing_ = label("", 8, y, 760, 44); y += 50;

    y = heading("Scenes (basics)", y);
    lbScene_ = listbox(8, y, 250, 110, [this](int i) { loadScene(i); });
    label("Name:", 270, y + 4, 50, 22); edSName_ = edit("", 322, y, 220, 28);
    label("Renderer:", 270, y + 38, 70, 22); cbRend_ = combo(344, y + 34, 200, [this](int) { fillParams(); }, 220);
    std::vector<std::string> rn; for (auto& r : show::rendererPack()) rn.push_back(r.id); comboFill(cbRend_, rn, 0);
    chkStrobe_ = check("Strobe-capable scene", 556, y, 210, nullptr);
    label("Param:", 270, y + 72, 52, 22); cbParam_ = combo(324, y + 68, 150, nullptr, 160); edPVal_ = edit("", 480, y + 68, 60, 28);
    button("Set param", 546, y + 66, 90, 30, [this] { sceneEdit(3); });
    y += 118;
    button("Add scene", 8, y, 100, 32, [this] { sceneEdit(0); });
    button("Apply changes", 114, y, 120, 32, [this] { sceneEdit(1); });
    button("Remove", 240, y, 90, 32, [this] { sceneEdit(2); });
    y += 44;

    y = heading("Cues (basics)", y);
    lbCue_ = listbox(8, y, 250, 110, [this](int i) { loadCue(i); });
    label("Scene:", 270, y + 4, 50, 22); cbCScene_ = combo(322, y, 220, nullptr, 200);
    label("Duration (s, 0 = hold):", 270, y + 38, 160, 22); edCDur_ = edit("0", 434, y + 34, 60, 28, ES_NUMBER | ES_RIGHT);
    label("Transition:", 270, y + 72, 80, 22); cbCTr_ = combo(352, y + 68, 110, nullptr, 130); comboFill(cbCTr_, {"cut", "fade", "dip_black", "wipe"}, 0);
    label("ms:", 470, y + 72, 30, 22); edCMs_ = edit("0", 502, y + 68, 60, 28, ES_NUMBER | ES_RIGHT);
    label("Text ref:", 570, y + 4, 60, 22); cbCText_ = combo(632, y, 136, nullptr, 160);
    y += 118;
    button("Add cue", 8, y, 90, 32, [this] { cueEdit(0); });
    button("Update selected", 104, y, 140, 32, [this] { cueEdit(1); });
    button("Remove", 250, y, 90, 32, [this] { cueEdit(2); });
    y += 44;

    y = heading("Text: Draft, Preview, Publish (audience outputs change only on Publish)", y);
    label("Draft (editing never reaches any audience output):", 8, y, 500, 22, true); y += 24;
    edDraft_ = edit("", 8, y, 500, 80, ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL | ES_AUTOVSCROLL);
    label("Saved message:", 520, y, 120, 22); cbMsg_ = combo(520, y + 24, 248, nullptr, 160);
    button("Load into draft", 520, y + 54, 120, 30, [this] { int i = comboSel(cbMsg_); auto d = ctl().document(); if (d && i >= 0 && i < (int)d->texts.size()) SetWindowTextW(edDraft_, W(crlf(d->texts[(size_t)i].text)).c_str()); });
    button("Save as message", 646, y + 54, 122, 30, [this] { saveMessage(); });
    y += 90;
    label("Publish to:", 8, y + 4, 80, 22); cbPub_ = combo(90, y, 190, nullptr, 160);
    button("Preview", 290, y - 2, 90, 32, [this] { doPreview(); }, "Copy the draft to the preview; audience outputs are unchanged");
    button("Publish", 386, y - 2, 90, 32, [this] { doPublish(); }, "Publish the previewed text to the chosen output(s)");
    button("Clear published text", 482, y - 2, 160, 32, [] { act(ActionId::ClearText); });
    y += 38;
    lblPrev_ = label("", 8, y, 760, 66); y += 70;

    y = heading("Overlays per logical output (what an audience output may show)", y);
    label("Output:", 8, y + 4, 60, 22); cbOvOut_ = combo(70, y, 180, [this](int) { ovLoaded_ = false; }, 160);
    chkOvText_ = check("Text", 262, y, 70, [this](bool) { ovEdit(); }); chkOvBpm_ = check("Tempo (BPM)", 340, y, 120, [this](bool) { ovEdit(); }); chkOvKey_ = check("Key", 466, y, 70, [this](bool) { ovEdit(); });
    y += 40;
    setContentHeight(y);
  }
  void activated() override { sig_.clear(); }
  void refresh() override {
    auto doc = ctl().document(); if (!doc) return;
    std::string sig = doc->name + fmt("|%zu|%zu|%zu|%zu|", doc->scenes.size(), doc->cues.size(), doc->media.size(), doc->texts.size()) + std::to_string(ctl().showSnapshot().revision);
    auto missing = ctl().missingMedia(); std::set<std::string> ms(missing.begin(), missing.end());
    rows_.clear(); size_t miss = 0;
    for (auto& m : doc->media) { bool gone = ms.count(m.hash) != 0; miss += gone; rows_.push_back({m.relPath, m.format, fmt("%llu B", (unsigned long long)m.bytes), gone ? "MISSING - placeholder shown" : "OK"}); }
    tableSet(tblMedia_, rows_);
    setTextIf(lblMissing_, miss ? fmt("%zu missing media file(s): the documented placeholder is shown instead; re-import or restore the files.", miss) : "No missing media.");
    if (sig != sig_) {
      sig_ = sig; auto sn = sceneNames(*doc);
      int ks = (int)SendMessageW(lbScene_, LB_GETCURSEL, 0, 0); listFill(lbScene_, numbered(sn), ks);
      std::vector<std::string> sc = {"(keep current scene)"}; for (auto& s : doc->scenes) sc.push_back(s.id);
      int kc = comboSel(cbCScene_); comboFill(cbCScene_, sc, kc < 0 ? 0 : kc);
      std::vector<std::string> tx = {"(none)"}; for (auto& t : doc->texts) tx.push_back(t.id); int kt = comboSel(cbCText_); comboFill(cbCText_, tx, kt < 0 ? 0 : kt);
      std::vector<std::string> cl; for (size_t i = 0; i < doc->cues.size(); ++i) cl.push_back(fmt("%d. ", (int)i + 1) + doc->cues[i].id + " -> " + (doc->cues[i].sceneId.empty() ? "(keep)" : doc->cues[i].sceneId));
      int kq = (int)SendMessageW(lbCue_, LB_GETCURSEL, 0, 0); listFill(lbCue_, cl, kq);
      std::vector<std::string> mm; for (auto& t : doc->texts) mm.push_back(t.id + ": " + t.text.substr(0, 40)); int km = comboSel(cbMsg_); comboFill(cbMsg_, mm, km);
      std::vector<std::string> po = {"All outputs"}, oo; pubIdx_ = {-1};
      for (size_t i = 0; i < doc->outputs.size(); ++i) { if (doc->outputs[i].role != show::OutputRole::Controls) { po.push_back(doc->outputs[i].logicalId); pubIdx_.push_back((int)i); } oo.push_back(doc->outputs[i].logicalId); }
      int kp = comboSel(cbPub_); comboFill(cbPub_, po, kp < 0 ? 0 : kp); int ko = comboSel(cbOvOut_); comboFill(cbOvOut_, oo, ko < 0 ? 0 : ko); ovLoaded_ = false;
    }
    if (!ovLoaded_) { int o = comboSel(cbOvOut_); if (o >= 0 && o < (int)doc->outputs.size()) { auto& ov = doc->outputs[(size_t)o].overlay; ovLoaded_ = true;
      SendMessageW(chkOvText_, BM_SETCHECK, ov.text, 0); SendMessageW(chkOvBpm_, BM_SETCHECK, ov.tempo, 0); SendMessageW(chkOvKey_, BM_SETCHECK, ov.key, 0); } }
    auto sn = ctl().showSnapshot(); std::string t = "Preview: " + (sn.previewText.empty() ? "(empty)" : sn.previewText) + "\r\n";
    for (auto& o : sn.outputs) if (o.role != show::OutputRole::Controls) t += "Published on " + o.logicalId + ": " + (o.publishedText.empty() ? "(nothing)" : o.publishedText) + "\r\n";
    setTextIf(lblPrev_, t);
  }
 private:
  static std::vector<std::string> numbered(const std::vector<std::string>& v) { std::vector<std::string> r; for (size_t i = 0; i < v.size(); ++i) r.push_back(fmt("%d. ", (int)i + 1) + v[i]); return r; }
  static std::string crlf(const std::string& s) { std::string r; for (char c : s) { if (c == '\n') r += "\r\n"; else r += c; } return r; }
  static std::string lf(const std::string& s) { std::string r; for (char c : s) if (c != '\r') r += c; return r; }
  void log(const std::string& l) { std::string t = getText(edLog_); t += (t.empty() ? "" : "\r\n") + l; SetWindowTextW(edLog_, W(t).c_str()); SendMessageW(edLog_, EM_SCROLL, SB_BOTTOM, 0); }

  void importMedia() {
    std::wstring first; std::vector<std::wstring> many;
    if (!openFileDialog(A.mainWnd, L"Import media", L"Media files\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.mp4;*.mov;*.mkv\0All files\0*.*\0", first, true, &many)) return;
    SetWindowTextW(edLog_, L"");
    for (auto& wp : many) importOne(U8(wp));
  }
  void importOne(const std::string& path) {
    std::string base = media::baseName(path);
    Status s = media::validateLocalPath(path); if (!s.ok()) { log("REJECTED " + base + ": " + s.detail); return; }
    media::AssetKind kind; s = media::classifyExtension(path, kind); if (!s.ok()) { log("REJECTED " + base + ": " + s.detail); return; }
    std::vector<uint8_t> head; uint64_t fsz = 0; s = media::readFileHead(path, 64, head, &fsz); if (!s.ok()) { log("REJECTED " + base + ": " + s.detail); return; }
    s = media::checkExtensionAndMagic(path, head.data(), head.size(), kind); if (!s.ok()) { log("REJECTED " + base + ": " + s.detail); return; }
    std::string note;
    if (kind == media::AssetKind::Still) { media::DecodedStill d; s = media::validateAndDecodeStillFile(path, media::limitsFor(media::Tier::Base), d); if (!s.ok()) { log("REJECTED " + base + ": " + s.detail); return; }
      note = fmt("%dx%d", d.srcW, d.srcH) + (d.downscaled ? " (will be scaled to fit 800x600)" : ""); for (auto& w : d.warnings) note += "; " + w; }
    else if (kind == media::AssetKind::Gif) { media::DecodedGif d; s = media::validateAndDecodeGifFile(path, media::limitsFor(media::Tier::Base), d); if (!s.ok()) { log("REJECTED " + base + ": " + s.detail); return; }
      note = fmt("%dx%d, %u frames", d.w, d.h, d.frameCount); for (auto& w : d.warnings) note += "; " + w; }
    else { if (!win::mediaFoundationAvailable()) { log("REJECTED " + base + ": video import needs Windows Media Foundation, which is not available on this system. Use stills or GIF."); return; }
      note = "video profile is verified when first decoded; an unsupported codec shows the placeholder"; }
    std::string hex; s = media::sha256FileHex(path, hex); if (!s.ok()) { log("REJECTED " + base + ": " + s.detail); return; }
    std::string rel; for (char c : base) rel += (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') ? c : '_';
    rel = "media/" + hex.substr(0, 8) + "_" + rel;
    std::string root = mediaRoot();
    CreateDirectoryW(W(root).c_str(), nullptr); CreateDirectoryW(W(root + "/media").c_str(), nullptr);
    if (!CopyFileW(W(path).c_str(), W(root + "/" + rel).c_str(), FALSE)) { log("REJECTED " + base + ": cannot copy into the media folder (check permissions and disk space)"); return; }
    show::MediaRecord m; m.hash = hex; m.relPath = rel; m.format = extOf(path); m.bytes = fsz;
    bool dup = false; applyDocEdit([&](show::ShowDocument& d) { for (auto& x : d.media) if (x.hash == hex) dup = true; if (!dup) d.media.push_back(m); });
    ctl().setMediaRoot(root);
    log((dup ? "Already imported: " : "Imported ") + base + " - " + note);
  }
  static std::string mediaRoot() { if (!A.showPath.empty()) { size_t p = A.showPath.find_last_of("\\/"); if (p != std::string::npos) return A.showPath.substr(0, p); } return A.dataDir + "/media-root"; }

  void loadScene(int i) {
    auto d = ctl().document(); if (!d || i < 0 || i >= (int)d->scenes.size()) return; const auto& s = d->scenes[(size_t)i];
    SetWindowTextW(edSName_, W(s.name).c_str());
    int ri = 0; for (size_t k = 0; k < show::rendererPack().size(); ++k) if (s.rendererType == show::rendererPack()[k].id) ri = (int)k;
    SendMessageW(cbRend_, CB_SETCURSEL, (WPARAM)ri, 0); SendMessageW(chkStrobe_, BM_SETCHECK, s.strobe, 0); fillParams();
  }
  void fillParams() {
    int ri = comboSel(cbRend_); std::vector<std::string> p; if (ri >= 0) for (auto& x : show::rendererPack()[(size_t)ri].params) p.push_back(x.name);
    comboFill(cbParam_, p, p.empty() ? -1 : 0);
  }
  void sceneEdit(int op) {
    int i = (int)SendMessageW(lbScene_, LB_GETCURSEL, 0, 0); std::string name = getText(edSName_); int ri = comboSel(cbRend_);
    bool strobe = SendMessageW(chkStrobe_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ri < 0) return; std::string rend = show::rendererPack()[(size_t)ri].id;
    if (op != 0 && i < 0) { postNotice("Select a scene first.", true); return; }
    int pi = comboSel(cbParam_); std::string pname; if (pi >= 0 && pi < (int)show::rendererPack()[(size_t)ri].params.size()) pname = show::rendererPack()[(size_t)ri].params[(size_t)pi].name;
    float pv = (float)atof(getText(edPVal_).c_str());
    applyDocEdit([&](show::ShowDocument& d) {
      if (op == 0) { show::Scene s; int n = (int)d.scenes.size() + 1; std::set<std::string> ids; for (auto& x : d.scenes) ids.insert(x.id);
        while (ids.count("scene" + std::to_string(n))) ++n; s.id = "scene" + std::to_string(n); s.name = name.empty() ? s.id : name; s.rendererType = rend; s.strobe = strobe; d.scenes.push_back(s); }
      else if (op == 1) { auto& s = d.scenes[(size_t)i]; s.name = name; if (s.rendererType != rend) s.params.clear(); s.rendererType = rend; s.strobe = strobe; }
      else if (op == 2) { std::string id = d.scenes[(size_t)i].id; d.scenes.erase(d.scenes.begin() + i);
        for (auto& c : d.cues) if (c.sceneId == id) c.sceneId.clear(); d.autoCfg.pool.erase(std::remove_if(d.autoCfg.pool.begin(), d.autoCfg.pool.end(), [&](const show::PoolEntry& p) { return p.sceneId == id; }), d.autoCfg.pool.end()); }
      else if (op == 3 && !pname.empty()) d.scenes[(size_t)i].params[pname] = pv;
    });
  }
  void loadCue(int i) {
    auto d = ctl().document(); if (!d || i < 0 || i >= (int)d->cues.size()) return; const auto& c = d->cues[(size_t)i];
    int si = 0; for (size_t k = 0; k < d->scenes.size(); ++k) if (d->scenes[k].id == c.sceneId) si = (int)k + 1;
    SendMessageW(cbCScene_, CB_SETCURSEL, (WPARAM)si, 0); SetWindowTextW(edCDur_, W(fmt("%lld", (long long)(c.duration.ms / 1000))).c_str());
    int ti = 0; const char* tn[] = {"cut", "fade", "dip_black", "wipe"}; for (int k = 0; k < 4; ++k) if (c.transition.type == tn[k]) ti = k;
    SendMessageW(cbCTr_, CB_SETCURSEL, (WPARAM)ti, 0); SetWindowTextW(edCMs_, W(fmt("%d", c.transition.ms)).c_str());
    int xi = 0; for (size_t k = 0; k < d->texts.size(); ++k) if (d->texts[k].id == c.textRef) xi = (int)k + 1; SendMessageW(cbCText_, CB_SETCURSEL, (WPARAM)xi, 0);
  }
  void cueEdit(int op) {
    int i = (int)SendMessageW(lbCue_, LB_GETCURSEL, 0, 0); if (op != 0 && i < 0) { postNotice("Select a cue first.", true); return; }
    int si = comboSel(cbCScene_), ti = comboSel(cbCTr_), xi = comboSel(cbCText_); long long dur = atoll(getText(edCDur_).c_str()); int ms = atoi(getText(edCMs_).c_str());
    const char* tn[] = {"cut", "fade", "dip_black", "wipe"};
    applyDocEdit([&](show::ShowDocument& d) {
      auto fill = [&](show::Cue& c) { c.sceneId = (si > 0 && si - 1 < (int)d.scenes.size()) ? d.scenes[(size_t)si - 1].id : ""; c.duration.beats = false; c.duration.ms = dur * 1000; c.transition.type = tn[std::max(0, ti) % 4]; c.transition.ms = ms;
        c.textRef = (xi > 0 && xi - 1 < (int)d.texts.size()) ? d.texts[(size_t)xi - 1].id : ""; };
      if (op == 0) { show::Cue c; int n = (int)d.cues.size() + 1; std::set<std::string> ids; for (auto& x : d.cues) ids.insert(x.id); while (ids.count("cue" + std::to_string(n))) ++n; c.id = "cue" + std::to_string(n); fill(c); d.cues.push_back(c); }
      else if (op == 1) fill(d.cues[(size_t)i]); else d.cues.erase(d.cues.begin() + i);
    });
  }
  void saveMessage() {
    std::string t = lf(getText(edDraft_)); if (t.empty()) { postNotice("The draft is empty.", true); return; }
    applyDocEdit([&](show::ShowDocument& d) { show::TextMessage m; int n = (int)d.texts.size() + 1; std::set<std::string> ids; for (auto& x : d.texts) ids.insert(x.id); while (ids.count("msg" + std::to_string(n))) ++n; m.id = "msg" + std::to_string(n); m.text = t; d.texts.push_back(m); });
  }
  void doPreview() {
    std::string t = lf(getText(edDraft_)); auto f = ctl().setDraftText(t); Status s = waitStatus(f);
    if (!s.ok()) { postNotice("Draft rejected: " + s.detail, true); return; }
    auto p = ctl().previewDraftText(); s = waitStatus(p); postNotice(s.ok() ? "Preview updated. Audience outputs unchanged." : "Preview failed: " + s.detail, !s.ok());
  }
  void doPublish() {
    std::string t = lf(getText(edDraft_)); if (ctl().showSnapshot().previewText != t) { postNotice("Preview the current draft first, then publish.", true); return; }
    int pi = comboSel(cbPub_); int target = (pi >= 0 && pi < (int)pubIdx_.size()) ? pubIdx_[(size_t)pi] : -1;
    auto f = ctl().publishDraftText(target); Status s = waitStatus(f); postNotice(s.ok() ? "Text published." : "Publish failed: " + s.detail, !s.ok());
  }
  void ovEdit() {
    int o = comboSel(cbOvOut_); bool a = SendMessageW(chkOvText_, BM_GETCHECK, 0, 0) == BST_CHECKED, b = SendMessageW(chkOvBpm_, BM_GETCHECK, 0, 0) == BST_CHECKED, c = SendMessageW(chkOvKey_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (o < 0) return; applyDocEdit([&](show::ShowDocument& d) { if (o < (int)d.outputs.size()) { d.outputs[(size_t)o].overlay.text = a; d.outputs[(size_t)o].overlay.tempo = b; d.outputs[(size_t)o].overlay.key = c; } });
  }
  HWND edLog_ = nullptr, tblMedia_ = nullptr, lblMissing_ = nullptr, lbScene_ = nullptr, edSName_ = nullptr, cbRend_ = nullptr, chkStrobe_ = nullptr, cbParam_ = nullptr, edPVal_ = nullptr,
       lbCue_ = nullptr, cbCScene_ = nullptr, edCDur_ = nullptr, cbCTr_ = nullptr, edCMs_ = nullptr, cbCText_ = nullptr, edDraft_ = nullptr, cbMsg_ = nullptr, cbPub_ = nullptr, lblPrev_ = nullptr,
       cbOvOut_ = nullptr, chkOvText_ = nullptr, chkOvBpm_ = nullptr, chkOvKey_ = nullptr;
  std::string sig_; std::vector<std::vector<std::string>> rows_; std::vector<int> pubIdx_; bool ovLoaded_ = false;
};
} // namespace
std::unique_ptr<Page> makeMediaPage() { return std::unique_ptr<Page>(new MediaPage()); }
}} // namespace ss::ui
