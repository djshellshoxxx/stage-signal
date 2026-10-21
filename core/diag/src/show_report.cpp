#include "ss/diag/show_report.hpp"

#include <algorithm>
#include <map>
#include "ss/diag/text.hpp"
#include "ss/security/redact.hpp"

namespace ss::diag {
namespace {

// ---- analysis ----------------------------------------------------------------------------------
struct Note { Ns atNs = 0; std::int64_t wallMs = 0; EventKind kind = EventKind::Note; std::string detail; bool derived = false; };

struct Analysis {
    Ns t0 = 0, t1 = 0;
    std::vector<Event> ev;
    std::vector<FeatureSample> sm;
    std::vector<Note> operational, discontinuities;
    ReportSummary summary;
    std::vector<SetlistEntry> setlist;
    std::vector<SceneUsage> scenes;
    std::vector<DropEntry> drops;
    std::vector<std::pair<Ns, std::string>> markerLines;   // relative time, label
    bool truncated = false;
};

double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// most common (tonic, minor); ties go to the smaller code
bool dominantKey(const std::vector<const FeatureSample*>& s, int& tonic, bool& minor) {
    std::map<int, int> counts;
    for (const auto* p : s) if (p->keyTonic >= 0 && p->keyTonic <= 11) ++counts[p->keyTonic * 2 + (p->keyMinor ? 1 : 0)];
    int best = -1, bestN = 0;
    for (const auto& kv : counts) if (kv.second > bestN) { best = kv.first; bestN = kv.second; }
    if (best < 0) return false;
    tonic = best / 2; minor = best % 2 == 1;
    return true;
}

std::string sceneLabel(const std::map<std::uint32_t, std::string>& names, std::uint32_t id) {
    auto it = names.find(id);
    return it != names.end() && !it->second.empty() ? it->second : "Scene " + std::to_string(id);
}

Analysis analyse(const ReportLimits& lim, const std::vector<Event>& events, const std::vector<FeatureSample>& samples,
                 const std::map<std::uint32_t, std::string>& names, bool inputTruncated) {
    Analysis a;
    a.truncated = inputTruncated;
    a.ev = events;
    a.sm = samples;
    std::stable_sort(a.ev.begin(), a.ev.end(), [](const Event& x, const Event& y) { return x.monoNs != y.monoNs ? x.monoNs < y.monoNs : x.seq < y.seq; });
    std::stable_sort(a.sm.begin(), a.sm.end(), [](const FeatureSample& x, const FeatureSample& y) { return x.monoNs < y.monoNs; });
    if (a.ev.empty() && a.sm.empty()) return a;

    bool have = false;
    auto touch = [&](Ns t) { if (!have) { a.t0 = a.t1 = t; have = true; } else { a.t0 = std::min(a.t0, t); a.t1 = std::max(a.t1, t); } };
    for (const auto& e : a.ev) touch(e.monoNs);
    for (const auto& s : a.sm) touch(s.monoNs);
    const Ns dur = a.t1 - a.t0;
    ReportSummary& sum = a.summary;
    sum.durationNs = dur; sum.events = a.ev.size(); sum.samples = a.sm.size();
    if (!a.ev.empty()) sum.wallStartUnixMs = a.ev.front().wallUnixMs;

    // ---- scene usage ----
    struct Seg { std::uint32_t id; Ns start, end; };
    std::vector<Seg> segs;
    std::vector<const Event*> sceneEvents;
    for (const auto& e : a.ev) if (e.kind == EventKind::Scene) sceneEvents.push_back(&e);
    if (!sceneEvents.empty()) {
        for (std::size_t i = 0; i < sceneEvents.size(); ++i)
            segs.push_back({sceneEvents[i]->id, sceneEvents[i]->monoNs, i + 1 < sceneEvents.size() ? sceneEvents[i + 1]->monoNs : a.t1});
    } else if (!a.sm.empty()) {
        std::size_t i = 0;
        while (i < a.sm.size()) {
            std::size_t j = i;
            while (j < a.sm.size() && a.sm[j].sceneId == a.sm[i].sceneId) ++j;
            const Ns end = j < a.sm.size() ? a.sm[j].monoNs : std::min(a.sm.back().monoNs + kMinSampleIntervalNs, std::max(a.t1, a.sm.back().monoNs));
            segs.push_back({a.sm[i].sceneId, a.sm[i].monoNs, std::max(end, a.sm[i].monoNs)});
            i = j;
        }
    }
    {
        std::map<std::uint32_t, SceneUsage> agg;
        for (const auto& s : segs) {
            auto& u = agg[s.id];
            u.id = s.id; ++u.activations; u.totalNs += s.end - s.start;
        }
        for (auto& kv : agg) {
            kv.second.name = security::sanitizeForLog(sceneLabel(names, kv.first), 80);
            kv.second.permille = dur > 0 ? static_cast<int>(std::min<Ns>(1000, kv.second.totalNs * 1000 / dur)) : 0;
            a.scenes.push_back(kv.second);
        }
        std::stable_sort(a.scenes.begin(), a.scenes.end(), [](const SceneUsage& x, const SceneUsage& y) { return x.totalNs != y.totalNs ? x.totalNs > y.totalNs : x.id < y.id; });
        sum.scenesUsed = a.scenes.size();
    }

    // ---- setlist sections from markers ----
    std::vector<const Event*> markers;
    for (const auto& e : a.ev) if (e.kind == EventKind::Marker) markers.push_back(&e);
    sum.markers = markers.size();
    if (markers.size() > lim.maxMarkers) { markers.resize(lim.maxMarkers); a.truncated = true; }
    struct Sec { std::string label; Ns start, end; bool implicit; };
    std::vector<Sec> secs;
    if (!markers.empty()) {
        for (std::size_t i = 0; i < markers.size(); ++i)
            secs.push_back({markers[i]->detail, markers[i]->monoNs, i + 1 < markers.size() ? markers[i + 1]->monoNs : a.t1, false});
    } else {
        secs.push_back({"Session", a.t0, a.t1, true});
    }
    for (std::size_t i = 0; i < secs.size(); ++i) {
        const Sec& s = secs[i];
        const bool last = i + 1 == secs.size();
        std::vector<double> bpms;
        std::vector<const FeatureSample*> inSec;
        float peak = 0.f;
        for (const auto& f : a.sm) {
            if (f.monoNs < s.start || f.monoNs > s.end || (f.monoNs == s.end && !last)) continue;
            inSec.push_back(&f);
            if (f.bpm > 0.0) bpms.push_back(f.bpm);
            peak = std::max(peak, f.energyArc);
        }
        SetlistEntry e;
        e.index = i + 1; e.label = s.label; e.startNs = s.start - a.t0; e.endNs = s.end - a.t0; e.medianBpm = medianOf(bpms);
        e.peakEnergy = peak; e.sampleCount = inSec.size(); e.implicitSection = s.implicit;
        dominantKey(inSec, e.keyTonic, e.keyMinor);
        a.setlist.push_back(std::move(e));
        a.markerLines.push_back({s.start - a.t0, s.label});
    }
    if (secs.size() == 1 && secs[0].implicit) a.markerLines.clear();

    // ---- drops (transitions into phase 2) ----
    {
        std::uint8_t prev = 0;
        bool first = true;
        for (const auto& f : a.sm) {
            if (f.arcPhase == 2 && (first || prev != 2)) {
                DropEntry d;
                d.atNs = f.monoNs - a.t0; d.energy = f.energyArc; d.bpm = f.bpm; d.sceneId = f.sceneId;
                d.sceneName = security::sanitizeForLog(sceneLabel(names, f.sceneId), 80);
                a.drops.push_back(std::move(d));
            }
            prev = f.arcPhase; first = false;
        }
        sum.drops = a.drops.size();
    }

    // ---- overall tempo / key ----
    {
        std::vector<double> bpms;
        std::vector<const FeatureSample*> all;
        for (const auto& f : a.sm) { all.push_back(&f); if (f.bpm > 0.0) bpms.push_back(f.bpm); }
        sum.medianBpm = medianOf(bpms);
        dominantKey(all, sum.keyTonic, sum.keyMinor);
    }

    // ---- operational events and discontinuities ----
    for (const auto& e : a.ev) {
        const bool op = e.kind == EventKind::Device || e.kind == EventKind::Adapt || e.kind == EventKind::Recording || e.kind == EventKind::Broadcast || e.kind == EventKind::Error;
        if (op) a.operational.push_back({e.monoNs - a.t0, e.wallUnixMs, e.kind, e.detail, false});
        if (e.kind == EventKind::Discontinuity) a.discontinuities.push_back({e.monoNs - a.t0, e.wallUnixMs, e.kind, e.detail, false});
    }
    for (std::size_t i = 1; i < a.sm.size(); ++i) {
        const Ns gap = a.sm[i].monoNs - a.sm[i - 1].monoNs;
        if (gap > lim.gapThresholdNs)
            a.discontinuities.push_back({a.sm[i - 1].monoNs - a.t0, 0, EventKind::Discontinuity, "no feature samples for " + fixedDecimals(nsToSec(gap), 0) + " s", true});
    }
    std::stable_sort(a.discontinuities.begin(), a.discontinuities.end(), [](const Note& x, const Note& y) { return x.atNs < y.atNs; });
    sum.discontinuities = a.discontinuities.size();
    return a;
}

// ---- HTML --------------------------------------------------------------------------------------
const char* kCss =
    ":root{--bg:#f7f7f4;--fg:#1d1f21;--muted:#5c6168;--line:#d9dad5;--card:#ffffff;--energy:#c2410c;--bpm:#1d4ed8;--mark:#6b7280;--drop:#b91c1c;--gap:#a16207}"
    "@media (prefers-color-scheme:dark){:root{--bg:#15171a;--fg:#e8e8e6;--muted:#a0a4ab;--line:#33363b;--card:#1d2024;--energy:#fb923c;--bpm:#7aa2ff;--mark:#9ca3af;--drop:#f87171;--gap:#fbbf24}}"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}"
    "main{max-width:1000px;margin:0 auto;padding:24px 16px 48px}h1{font-size:1.6rem;margin:0 0 4px}h2{font-size:1.1rem;margin:32px 0 8px}"
    ".meta{color:var(--muted);margin:0 0 16px}dl.sum{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px;margin:0}"
    "dl.sum div{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:8px 12px}dt{color:var(--muted);font-size:.8rem}dd{margin:0;font-size:1.15rem;font-variant-numeric:tabular-nums}"
    ".tw{overflow-x:auto;background:var(--card);border:1px solid var(--line);border-radius:8px}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}"
    "th,td{text-align:left;padding:6px 10px;border-bottom:1px solid var(--line);vertical-align:top}th{color:var(--muted);font-weight:600;font-size:.8rem}tr:last-child td{border-bottom:0}"
    "td.d{word-break:break-word}.note{color:var(--muted);font-size:.85rem;margin:6px 0 0}.chart{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:8px}"
    "svg{display:block;width:100%;height:auto}svg text{fill:var(--muted);font-size:11px}.e{fill:none;stroke:var(--energy);stroke-width:2}.b{fill:none;stroke:var(--bpm);stroke-width:2;stroke-dasharray:6 3}"
    ".mk{stroke:var(--mark);stroke-width:1;stroke-dasharray:3 3}.dr{fill:var(--drop);stroke:var(--card);stroke-width:1.5}.gp{stroke:var(--gap);stroke-width:3}.ax{stroke:var(--line);stroke-width:1}"
    ".lg{display:flex;flex-wrap:wrap;gap:4px 16px;color:var(--muted);font-size:.85rem;margin:6px 0 0;padding:0;list-style:none}footer{margin-top:32px;color:var(--muted);font-size:.8rem}"
    "@media print{body{background:#fff;color:#000}.tw,.chart,dl.sum div{border-color:#999}}";

std::string num1(double v) { return fixedDecimals(v, 1); }

struct Table {
    std::vector<std::string> heads;
    std::vector<std::vector<std::string>> rows;   // cells are already escaped HTML
    std::vector<std::string> classes;             // optional per-column class
};

std::string renderTable(const Table& t, std::size_t maxRows, bool& truncated) {
    std::string o = "<div class=\"tw\"><table><thead><tr>";
    for (const auto& h : t.heads) o += "<th>" + h + "</th>";
    o += "</tr></thead><tbody>";
    const std::size_t n = std::min(t.rows.size(), maxRows);
    for (std::size_t r = 0; r < n; ++r) {
        o += "<tr>";
        for (std::size_t c = 0; c < t.rows[r].size(); ++c) {
            const bool cls = c < t.classes.size() && !t.classes[c].empty();
            o += cls ? "<td class=\"" + t.classes[c] + "\">" : "<td>";
            o += t.rows[r][c] + "</td>";
        }
        o += "</tr>";
    }
    o += "</tbody></table></div>";
    if (t.rows.size() > n) {
        truncated = true;
        o += "<p class=\"note\">" + std::to_string(t.rows.size() - n) + " more row(s) not shown. The CSV export has everything.</p>";
    }
    return o;
}

std::string section(const std::string& title, const std::string& body) { return "<section><h2>" + htmlEscape(title) + "</h2>" + body + "</section>"; }
std::string none(const char* what) { return std::string("<p class=\"note\">") + what + "</p>"; }

std::string renderSvg(const Analysis& a, const ReportLimits& lim) {
    if (a.sm.empty()) return none("No feature samples were recorded, so there is no energy or BPM timeline.");
    const double W = 960, H = 300, L = 52, R = 52, T = 30, B = 40, pw = W - L - R, ph = H - T - B;
    const Ns dur = std::max<Ns>(a.t1 - a.t0, 1);
    auto X = [&](double relMs) { return L + pw * relMs / (static_cast<double>(dur) / 1e6); };

    double bmin = 1e9, bmax = 0;
    for (const auto& f : a.sm) if (f.bpm > 0) { bmin = std::min(bmin, f.bpm); bmax = std::max(bmax, f.bpm); }
    const bool haveBpm = bmax > 0;
    if (haveBpm && bmax - bmin < 10) { const double mid = (bmax + bmin) / 2; bmin = mid - 5; bmax = mid + 5; }
    auto YE = [&](double e) { return T + ph * (1.0 - std::min(1.0, std::max(0.0, e))); };
    auto YB = [&](double b) { return T + ph * (1.0 - (b - bmin) / (bmax - bmin)); };

    const std::size_t buckets = std::max<std::size_t>(1, std::min(a.sm.size(), lim.maxChartPoints));
    struct Bk { std::int64_t tMsSum = 0; std::size_t n = 0; double eSum = 0; double bSum = 0; std::size_t bN = 0; };
    std::vector<Bk> bk(buckets);
    for (const auto& f : a.sm) {
        std::size_t i = static_cast<std::size_t>(static_cast<double>(f.monoNs - a.t0) / static_cast<double>(dur + 1) * static_cast<double>(buckets));
        if (i >= buckets) i = buckets - 1;
        Bk& b = bk[i];
        b.tMsSum += (f.monoNs - a.t0) / kNsPerMs; ++b.n; b.eSum += f.energyArc;
        if (f.bpm > 0) { b.bSum += f.bpm; ++b.bN; }
    }
    std::string eP, bP;
    for (const auto& b : bk) {
        if (!b.n) continue;
        const double x = X(static_cast<double>(b.tMsSum) / static_cast<double>(b.n));
        eP += num1(x) + "," + num1(YE(b.eSum / static_cast<double>(b.n))) + " ";
        if (b.bN) bP += num1(x) + "," + num1(YB(b.bSum / static_cast<double>(b.bN))) + " ";
    }

    std::string s = "<svg viewBox=\"0 0 960 300\" role=\"img\" aria-labelledby=\"tl-t tl-d\"><title id=\"tl-t\">Energy and BPM timeline</title>"
                    "<desc id=\"tl-d\">Energy arc from 0 to 1 (solid) and tempo in BPM (dashed) over the session, with markers, drops and gaps.</desc>";
    s += "<rect x=\"" + num1(L) + "\" y=\"" + num1(T) + "\" width=\"" + num1(pw) + "\" height=\"" + num1(ph) + "\" fill=\"none\" class=\"ax\"/>";
    for (int k = 0; k <= 4; ++k) {
        const double x = L + pw * k / 4.0;
        s += "<line class=\"ax\" x1=\"" + num1(x) + "\" y1=\"" + num1(T + ph) + "\" x2=\"" + num1(x) + "\" y2=\"" + num1(T + ph + 4) + "\"/>";
        s += "<text x=\"" + num1(x) + "\" y=\"" + num1(T + ph + 18) + "\" text-anchor=\"middle\">" + htmlEscape(formatDuration(dur * k / 4)) + "</text>";
    }
    for (double e : {0.0, 0.5, 1.0})
        s += "<text x=\"" + num1(L - 6) + "\" y=\"" + num1(YE(e) + 4) + "\" text-anchor=\"end\">" + fixedDecimals(e, 1) + "</text>";
    if (haveBpm) {
        s += "<text x=\"" + num1(W - R + 6) + "\" y=\"" + num1(T + 4) + "\">" + fixedDecimals(bmax, 0) + "</text>";
        s += "<text x=\"" + num1(W - R + 6) + "\" y=\"" + num1(T + ph + 4) + "\">" + fixedDecimals(bmin, 0) + "</text>";
    }
    std::size_t mi = 0;
    for (const auto& m : a.markerLines) {
        const double x = X(static_cast<double>(m.first) / 1e6);
        s += "<line class=\"mk\" x1=\"" + num1(x) + "\" y1=\"" + num1(T) + "\" x2=\"" + num1(x) + "\" y2=\"" + num1(T + ph) + "\"/>";
        s += "<text x=\"" + num1(x + 3) + "\" y=\"" + num1(11.0 + static_cast<double>(mi % 2) * 12.0) + "\">" + htmlEscape(security::truncateUtf8(m.second, 28)) + "</text>";
        ++mi;
    }
    if (!eP.empty()) s += "<polyline class=\"e\" points=\"" + eP + "\"/>";
    if (!bP.empty()) s += "<polyline class=\"b\" points=\"" + bP + "\"/>";
    for (const auto& d : a.drops) s += "<circle class=\"dr\" cx=\"" + num1(X(static_cast<double>(d.atNs) / 1e6)) + "\" cy=\"" + num1(YE(d.energy)) + "\" r=\"4.5\"/>";
    for (const auto& g : a.discontinuities) {
        const double x = X(static_cast<double>(g.atNs) / 1e6);
        s += "<line class=\"gp\" x1=\"" + num1(x) + "\" y1=\"" + num1(T + ph - 8) + "\" x2=\"" + num1(x) + "\" y2=\"" + num1(T + ph) + "\"/>";
    }
    s += "</svg>";
    return "<div class=\"chart\">" + s + "</div><ul class=\"lg\"><li>Solid line: energy arc (0 to 1)</li><li>Dashed line: tempo (BPM)</li>"
           "<li>Dotted verticals: markers</li><li>Dots: drops</li><li>Short ticks at the bottom: gaps and discontinuities</li></ul>";
}

std::string keyCell(int tonic, bool minor) { const auto k = keyName(tonic, minor); return k.empty() ? "&ndash;" : htmlEscape(k); }
std::string bpmCell(double bpm) { return bpm > 0 ? num1(bpm) : "&ndash;"; }

std::string renderHtml(const Analysis& a, const ReportLimits& lim, const std::string& title, const std::string& version, std::int64_t generatedMs,
                       std::size_t rows, bool& truncated) {
    const std::string safeTitle = htmlEscape(security::sanitizeForLog(title, 120));
    std::string h = "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                    "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\"><title>Show report - " + safeTitle + "</title><style>";
    h += kCss;
    h += "</style></head><body><main><h1>" + safeTitle + "</h1><p class=\"meta\">Show report";
    if (!version.empty()) h += " &middot; StageSignal " + htmlEscape(security::sanitizeForLog(version, 40));
    if (a.summary.wallStartUnixMs != 0) h += " &middot; session start " + htmlEscape(formatUtcIso(a.summary.wallStartUnixMs)) + " UTC";
    if (generatedMs != 0) h += " &middot; generated " + htmlEscape(formatUtcIso(generatedMs)) + " UTC";
    h += "</p>";

    if (a.ev.empty() && a.sm.empty()) {
        h += "<p>No events or feature samples were recorded for this session.</p>";
    } else {
        const ReportSummary& s = a.summary;
        auto stat = [](const std::string& k, const std::string& v) { return "<div><dt>" + k + "</dt><dd>" + v + "</dd></div>"; };
        h += "<dl class=\"sum\">" + stat("Duration", htmlEscape(formatDuration(s.durationNs))) + stat("Median BPM", bpmCell(s.medianBpm)) +
             stat("Most common key", keyCell(s.keyTonic, s.keyMinor)) + stat("Markers", std::to_string(s.markers)) + stat("Drops", std::to_string(s.drops)) +
             stat("Scenes used", std::to_string(s.scenesUsed)) + stat("Events", std::to_string(s.events)) + stat("Gaps", std::to_string(s.discontinuities)) + "</dl>";
        h += section("Energy and BPM timeline", renderSvg(a, lim));

        Table sl; sl.heads = {"#", "Section", "Start", "Length", "Median BPM", "Key", "Peak energy"};
        for (const auto& e : a.setlist)
            sl.rows.push_back({std::to_string(e.index), htmlEscape(e.label) + (e.implicitSection ? " <span class=\"note\">(no markers set)</span>" : ""), htmlEscape(formatDuration(e.startNs)),
                               htmlEscape(formatDuration(e.endNs - e.startNs)), bpmCell(e.medianBpm), keyCell(e.keyTonic, e.keyMinor), fixedDecimals(e.peakEnergy, 2)});
        sl.classes = {"", "d"};
        h += section("Setlist and markers", a.setlist.empty() ? none("No markers recorded.") : renderTable(sl, rows, truncated));

        Table su; su.heads = {"Scene", "Activations", "Time on screen", "Share"};
        for (const auto& u : a.scenes)
            su.rows.push_back({htmlEscape(u.name), std::to_string(u.activations), htmlEscape(formatDuration(u.totalNs)), fixedDecimals(u.permille / 10.0, 1) + "%"});
        su.classes = {"d"};
        h += section("Scene usage", a.scenes.empty() ? none("No scene changes recorded.") : renderTable(su, rows, truncated));

        Table dr; dr.heads = {"Time", "Energy", "BPM", "Scene"};
        for (const auto& d : a.drops) dr.rows.push_back({htmlEscape(formatDuration(d.atNs)), fixedDecimals(d.energy, 2), bpmCell(d.bpm), htmlEscape(d.sceneName)});
        dr.classes = {"", "", "", "d"};
        h += section("Drops", a.drops.empty() ? none("No drops detected.") : renderTable(dr, rows, truncated));

        Table op; op.heads = {"Time", "UTC", "Type", "Detail"};
        for (const auto& n : a.operational)
            op.rows.push_back({htmlEscape(formatDuration(n.atNs)), n.wallMs ? htmlEscape(formatUtcIso(n.wallMs)) : "&ndash;", htmlEscape(kindLabel(n.kind)), htmlEscape(n.detail)});
        op.classes = {"", "", "", "d"};
        h += section("Device, recording and broadcast events", a.operational.empty() ? none("None recorded.") : renderTable(op, rows, truncated));

        Table dc; dc.heads = {"Time", "Source", "Detail"};
        for (const auto& n : a.discontinuities) dc.rows.push_back({htmlEscape(formatDuration(n.atNs)), n.derived ? "derived from samples" : "journal", htmlEscape(n.detail)});
        dc.classes = {"", "", "d"};
        h += section("Discontinuities", a.discontinuities.empty() ? none("None recorded.") : renderTable(dc, rows, truncated));
    }
    h += "<footer>Self-contained report: no scripts and no external resources. Secrets are removed before anything is journaled.</footer></main></body></html>";
    return h;
}

// ---- CSV ---------------------------------------------------------------------------------------
std::string eventsCsv(const Analysis& a, const ReportLimits& lim, bool& truncated) {
    std::string o = "seq,time_s,mono_ns,wall_utc,kind,id,detail\r\n";
    for (const auto& e : a.ev) {
        std::string row = csvNumber(std::to_string(e.seq)) + "," + csvNumber(fixedDecimals(nsToSec(e.monoNs - a.t0), 3)) + "," + csvNumber(std::to_string(e.monoNs)) + "," +
                          (e.wallUnixMs ? formatUtcIso(e.wallUnixMs, true) : std::string()) + "," + kindName(e.kind) + "," + csvNumber(std::to_string(e.id)) + "," +
                          csvText(security::sanitizeForLog(e.detail, kJournalDetailMaxBytes)) + "\r\n";
        if (o.size() + row.size() > lim.maxCsvBytes) { truncated = true; break; }
        o += row;
    }
    return o;
}

std::string setlistCsv(const Analysis& a) {
    std::string o = "index,label,start_s,end_s,duration_s,median_bpm,key,peak_energy,samples,implicit\r\n";
    for (const auto& e : a.setlist)
        o += csvNumber(std::to_string(e.index)) + "," + csvText(e.label) + "," + csvNumber(fixedDecimals(nsToSec(e.startNs), 1)) + "," + csvNumber(fixedDecimals(nsToSec(e.endNs), 1)) + "," +
             csvNumber(fixedDecimals(nsToSec(e.endNs - e.startNs), 1)) + "," + (e.medianBpm > 0 ? csvNumber(fixedDecimals(e.medianBpm, 1)) : std::string()) + "," +
             csvText(keyName(e.keyTonic, e.keyMinor)) + "," + csvNumber(fixedDecimals(e.peakEnergy, 2)) + "," + csvNumber(std::to_string(e.sampleCount)) + "," +
             (e.implicitSection ? "yes" : "no") + "\r\n";
    return o;
}

}  // namespace

ShowReportBuilder& ShowReportBuilder::addEvents(const std::vector<Event>& events) {
    for (const auto& e : events) {
        if (events_.size() >= limits_.maxEvents) { inputTruncated_ = true; break; }
        events_.push_back(e);
    }
    return *this;
}

ShowReportBuilder& ShowReportBuilder::addSamples(const std::vector<FeatureSample>& samples) {
    for (const auto& s : samples) {
        if (samples_.size() >= limits_.maxSamples) { inputTruncated_ = true; break; }
        samples_.push_back(s);
    }
    return *this;
}

Result<ShowReport> ShowReportBuilder::build() const {
    std::map<std::uint32_t, std::string> names;
    for (const auto& n : sceneNames_) names[n.id] = n.name;
    std::vector<Event> ev = events_;
    for (auto& e : ev) e.detail = security::sanitizeForLog(e.detail, kJournalDetailMaxBytes);   // defence in depth: callers may hand in raw events
    Analysis a = analyse(limits_, ev, samples_, names, inputTruncated_);

    ShowReport r;
    bool truncated = a.truncated;
    std::size_t rows = std::max<std::size_t>(limits_.maxTableRows, 10);
    for (;;) {
        bool t = false;
        r.html = renderHtml(a, limits_, title_, appVersion_, generatedAtMs_, rows, t);
        if (r.html.size() <= limits_.maxHtmlBytes) { truncated |= t; break; }
        if (rows <= 10) return makeError(ErrorCategory::ResourceExhausted, "show report exceeds the size limit even with minimal tables");
        rows /= 2;
        truncated = true;
    }
    r.eventsCsv = eventsCsv(a, limits_, truncated);
    r.setlistCsv = setlistCsv(a);
    r.summary = a.summary;
    r.setlist = a.setlist;
    r.sceneUsage = a.scenes;
    r.drops = a.drops;
    r.truncated = truncated;
    return r;
}

}  // namespace ss::diag
