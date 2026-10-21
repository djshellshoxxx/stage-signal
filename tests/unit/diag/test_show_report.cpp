#include "doctest/doctest.h"
#include <algorithm>
#include "ss/diag/show_report.hpp"
#include "ss/diag/text.hpp"

using namespace ss;
using namespace ss::diag;

static bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }
static std::size_t countOf(const std::string& h, const std::string& n) {
    std::size_t c = 0, p = 0;
    while ((p = h.find(n, p)) != std::string::npos) { ++c; p += n.size(); }
    return c;
}

namespace {
constexpr std::int64_t kWall0 = 1'700'000'000'000LL;   // 2023-11-14T22:13:20Z

Event ev(std::uint64_t seq, Ns tSec, EventKind k, const std::string& detail, std::uint32_t id = 0) {
    Event e; e.seq = seq; e.monoNs = secToNs(tSec); e.wallUnixMs = kWall0 + tSec * 1000; e.kind = k; e.detail = detail; e.id = id; return e;
}
FeatureSample smp(Ns tSec, double bpm, int tonic, bool minor, float energy, std::uint8_t phase, std::uint32_t scene) {
    FeatureSample s; s.monoNs = secToNs(tSec); s.bpm = bpm; s.keyTonic = tonic; s.keyMinor = minor; s.energyArc = energy; s.arcPhase = phase; s.sceneId = scene; return s;
}

// 180 s session: Song A (0-90 s) 128 BPM A minor, Song B (90-180 s) 140 BPM C major with a drop at 150 s.
struct Session {
    std::vector<Event> events;
    std::vector<FeatureSample> samples;
    Session() {
        std::uint64_t seq = 1;
        events.push_back(ev(seq++, 0, EventKind::Marker, "Song A"));
        events.push_back(ev(seq++, 0, EventKind::Scene, "to Neon Grid", 1));
        events.push_back(ev(seq++, 5, EventKind::Device, "audio input opened"));
        events.push_back(ev(seq++, 30, EventKind::Scene, "to Tunnel", 2));
        events.push_back(ev(seq++, 60, EventKind::Recording, "recording started"));
        events.push_back(ev(seq++, 90, EventKind::Marker, "Song B"));
        events.push_back(ev(seq++, 90, EventKind::Scene, "to Neon Grid", 1));
        events.push_back(ev(seq++, 120, EventKind::Discontinuity, "audio discontinuity 12 ms"));
        events.push_back(ev(seq++, 130, EventKind::Broadcast, "stream started"));
        events.push_back(ev(seq++, 175, EventKind::Device, "audio input lost"));
        for (int t = 0; t < 180; ++t) {
            if (t >= 100 && t < 104) continue;   // 4 s hole: below the 5 s derived-gap threshold
            const bool songA = t < 90;
            const std::uint8_t phase = (t >= 150 && t < 155) ? 2 : (t >= 140 && t < 150) ? 1 : 0;
            const float energy = songA ? 0.3f + 0.004f * static_cast<float>(t) : (phase == 2 ? 0.95f : 0.6f);
            samples.push_back(smp(t, songA ? 128.0 : 140.0, songA ? 9 : 0, songA, energy, phase, t < 30 ? 1 : t < 90 ? 2 : 1));
        }
    }
};

ShowReport build(const Session& s, ReportLimits lim = ReportLimits{}) {
    ShowReportBuilder b(lim);
    b.setTitle("Friday night").setAppVersion("0.1.0").setGeneratedAtWallMs(kWall0 + 200'000)
        .setSceneNames({{1, "Neon Grid"}, {2, "Tunnel"}}).addEvents(s.events).addSamples(s.samples);
    auto r = b.build();
    REQUIRE(r.ok());
    return std::move(r.value());
}
}  // namespace

TEST_CASE("DEC-13 Show Report: setlist sections carry median BPM and dominant key per marker") {
    const auto r = build(Session{});
    REQUIRE(r.setlist.size() == 2);
    CHECK(r.setlist[0].label == "Song A");
    CHECK(r.setlist[0].startNs == 0);
    CHECK(r.setlist[0].endNs == secToNs(90));
    CHECK(r.setlist[0].medianBpm == doctest::Approx(128.0));
    CHECK(keyName(r.setlist[0].keyTonic, r.setlist[0].keyMinor) == "A minor");
    CHECK(r.setlist[1].label == "Song B");
    CHECK(r.setlist[1].medianBpm == doctest::Approx(140.0));
    CHECK(keyName(r.setlist[1].keyTonic, r.setlist[1].keyMinor) == "C major");
    CHECK(r.setlist[1].peakEnergy == doctest::Approx(0.95f));
    CHECK_FALSE(r.setlist[0].implicitSection);
    CHECK(r.summary.durationNs == secToNs(179));
    CHECK(r.summary.markers == 2);
    CHECK(r.summary.wallStartUnixMs == kWall0);
}

TEST_CASE("DEC-13 Show Report: scene usage table, drops and discontinuities") {
    const auto r = build(Session{});
    REQUIRE(r.sceneUsage.size() == 2);
    // Neon Grid: 0-30 (30 s) + 90-179 (89 s) = 119 s, Tunnel: 30-90 = 60 s
    CHECK(r.sceneUsage[0].name == "Neon Grid");
    CHECK(r.sceneUsage[0].activations == 2);
    CHECK(r.sceneUsage[0].totalNs == secToNs(119));
    CHECK(r.sceneUsage[1].name == "Tunnel");
    CHECK(r.sceneUsage[1].totalNs == secToNs(60));
    CHECK(r.sceneUsage[0].permille == 664);   // 119 / 179
    REQUIRE(r.drops.size() == 1);
    CHECK(r.drops[0].atNs == secToNs(150));
    CHECK(r.drops[0].energy == doctest::Approx(0.95f));
    CHECK(r.drops[0].sceneName == "Neon Grid");
    CHECK(r.summary.drops == 1);
    CHECK(r.summary.discontinuities == 1);   // the journal event; the 4 s hole is under the gap threshold
}

TEST_CASE("DEC-13 Show Report: HTML is self-contained with the expected sections and golden substrings") {
    const auto r = build(Session{});
    const auto& h = r.html;
    CHECK(h.rfind("<!DOCTYPE html>", 0) == 0);
    CHECK(has(h, "<title>Show report - Friday night</title>"));
    CHECK(has(h, "<h1>Friday night</h1>"));
    CHECK(has(h, "StageSignal 0.1.0"));
    CHECK(has(h, "session start 2023-11-14T22:13:20Z UTC"));
    CHECK(has(h, "generated 2023-11-14T22:16:40Z UTC"));
    for (const char* s : {"Energy and BPM timeline", "Setlist and markers", "Scene usage", "<h2>Drops</h2>", "Device, recording and broadcast events", "<h2>Discontinuities</h2>"})
        CHECK(has(h, s));
    CHECK(has(h, "<svg viewBox=\"0 0 960 300\""));
    CHECK(has(h, "<polyline class=\"e\""));
    CHECK(has(h, "<polyline class=\"b\""));
    CHECK(has(h, "<circle class=\"dr\""));
    CHECK(has(h, "<line class=\"mk\""));
    CHECK(has(h, "<td>1</td><td class=\"d\">Song A</td><td>0:00</td><td>1:30</td><td>128.0</td><td>A minor</td>"));
    CHECK(has(h, "<td>2</td><td class=\"d\">Song B</td><td>1:30</td><td>1:29</td><td>140.0</td><td>C major</td><td>0.95</td>"));
    CHECK(has(h, "<td class=\"d\">Neon Grid</td><td>2</td><td>1:59</td><td>66.4%</td>"));
    CHECK(has(h, "<td>2:30</td><td>0.95</td><td>140.0</td><td class=\"d\">Neon Grid</td>"));
    CHECK(has(h, "audio input lost"));
    CHECK(has(h, "stream started"));
    CHECK(has(h, "audio discontinuity 12 ms"));
    CHECK(has(h, "<dt>Median BPM</dt><dd>"));
    CHECK(has(h, "<dt>Most common key</dt>"));
    // no scripts, no external resources, no remote references
    for (const char* bad : {"<script", "javascript:", "http://", "https://", "src=", "href=", "@import", "url(", "<link", "<iframe", "<img", "<object", "<embed", " onload=", " onerror=", " onclick="})
        { INFO(bad); CHECK_FALSE(has(h, bad)); }
    CHECK(has(h, "Content-Security-Policy"));
    CHECK(has(h, "default-src 'none'"));
    CHECK(has(h, "prefers-color-scheme:dark"));
    CHECK(has(h, "viewBox"));
}

TEST_CASE("DEC-13 Show Report: output is deterministic for the same input and independent of input order") {
    const Session s;
    const auto a = build(s);
    const auto b = build(s);
    CHECK(a.html == b.html);
    CHECK(a.eventsCsv == b.eventsCsv);
    CHECK(a.setlistCsv == b.setlistCsv);
    Session shuffled = s;
    std::reverse(shuffled.events.begin(), shuffled.events.end());
    std::reverse(shuffled.samples.begin(), shuffled.samples.end());
    const auto c = build(shuffled);
    CHECK(c.html == a.html);
    CHECK(c.eventsCsv == a.eventsCsv);
}

TEST_CASE("DEC-13 Show Report: XSS payloads in markers, scene names, titles and details are escaped everywhere") {
    Session s;
    s.events.push_back(ev(100, 10, EventKind::Marker, "<script>alert(1)</script>"));
    s.events.push_back(ev(101, 11, EventKind::Device, "</td><td onclick=\"x()\">\"><img src=x onerror=alert(2)>"));
    s.events.push_back(ev(102, 12, EventKind::Error, "javascript:alert(3) & <svg onload=alert(4)>"));
    ShowReportBuilder b;
    b.setTitle("</title><script>alert(5)</script>").setAppVersion("<b>1</b>")
        .setSceneNames({{1, "\"><img src=x onerror=alert(6)>"}, {2, "<svg/onload=alert(7)>"}}).addEvents(s.events).addSamples(s.samples);
    const auto r = b.build();
    REQUIRE(r.ok());
    const auto& h = r.value().html;
    for (const char* bad : {"<script", "<img", "<svg/onload", "<td onclick", "<b>1</b>"}) { INFO(bad); CHECK_FALSE(has(h, bad)); }
    CHECK(has(h, "&lt;script&gt;alert(1)&lt;/script&gt;"));
    CHECK(has(h, "&lt;/td&gt;&lt;td onclick=&quot;x()&quot;&gt;"));
    CHECK(has(h, "&lt;svg/onload=alert(7)&gt;"));
    CHECK(has(h, "&lt;b&gt;1&lt;/b&gt;"));
    // the only tags in the document are the ones the renderer wrote: count script/img/iframe-like openers
    CHECK(countOf(h, "</title>") == 2);   // page title + SVG title, the injected one is escaped
    CHECK(countOf(h, "<script") == 0);
    CHECK(countOf(h, "<svg") == 1);        // exactly the chart
    CHECK(countOf(h, "<style>") == 1);
    // the marker label inside the SVG is escaped too
    CHECK(has(h, "&lt;script&gt;alert(1)&lt;/scr"));
}

TEST_CASE("DEC-13 Show Report: CSV exports are RFC 4180 and neutralise spreadsheet formulas") {
    Session s;
    s.events.push_back(ev(100, 20, EventKind::Marker, "=HYPERLINK(\"http://evil\",\"x\")"));
    s.events.push_back(ev(101, 21, EventKind::Note, "has, comma and \"quotes\"\nand newline"));
    s.events.push_back(ev(102, 22, EventKind::Note, "+cmd|' /C calc'!A0"));
    const auto r = build(s);
    const auto& e = r.eventsCsv;
    CHECK(e.rfind("seq,time_s,mono_ns,wall_utc,kind,id,detail\r\n", 0) == 0);
    CHECK(has(e, "1,0.000,0,2023-11-14T22:13:20.000Z,marker,0,Song A\r\n"));
    CHECK(has(e, ",marker,0,\"'=HYPERLINK(\"\"http://evil\"\",\"\"x\"\")\"\r\n"));
    CHECK(has(e, ",note,0,\"has, comma and \"\"quotes\"\" and newline\"\r\n"));   // newline flattened by the journal sanitiser
    CHECK(has(e, ",note,0,'+cmd|' /C calc'!A0\r\n"));
    CHECK(countOf(e, "\r\n") == 1 + r.summary.events);
    const auto base = build(Session{});
    const auto& l = base.setlistCsv;
    CHECK(l.rfind("index,label,start_s,end_s,duration_s,median_bpm,key,peak_energy,samples,implicit\r\n", 0) == 0);
    CHECK(has(l, "1,Song A,0.0,90.0,90.0,128.0,A minor,"));
    CHECK(has(l, "2,Song B,90.0,"));
    CHECK(has(l, "140.0,C major,0.95,"));
    // marker with a formula becomes a setlist label: neutralised there too
    CHECK(has(r.setlistCsv, ",\"'=HYPERLINK("));
}

TEST_CASE("DEC-13 Show Report: no markers gives one implicit Session section; no samples gives a notice, not a chart") {
    ShowReportBuilder b;
    b.addEvents({ev(1, 0, EventKind::Scene, "x", 1), ev(2, 10, EventKind::Device, "d")});
    auto r = b.build();
    REQUIRE(r.ok());
    REQUIRE(r.value().setlist.size() == 1);
    CHECK(r.value().setlist[0].implicitSection);
    CHECK(r.value().setlist[0].label == "Session");
    CHECK(has(r.value().html, "no markers set"));
    CHECK(has(r.value().html, "No feature samples were recorded"));
    CHECK_FALSE(has(r.value().html, "<svg"));
    CHECK(has(r.value().setlistCsv, "1,Session,0.0,10.0,10.0,,,0.00,0,yes"));
}

TEST_CASE("DEC-13 Show Report: empty input produces a valid report with an explanation") {
    ShowReportBuilder b;
    auto r = b.build();
    REQUIRE(r.ok());
    CHECK(has(r.value().html, "No events or feature samples were recorded"));
    CHECK(r.value().eventsCsv == "seq,time_s,mono_ns,wall_utc,kind,id,detail\r\n");
    CHECK(r.value().setlist.empty());
    CHECK_FALSE(r.value().truncated);
}

TEST_CASE("DEC-13 Show Report: feature gaps longer than the threshold are reported as derived discontinuities") {
    Session s;
    s.samples.erase(std::remove_if(s.samples.begin(), s.samples.end(), [](const FeatureSample& f) { return f.monoNs >= secToNs(40) && f.monoNs < secToNs(50); }), s.samples.end());
    const auto r = build(s);
    CHECK(r.summary.discontinuities == 2);
    CHECK(has(r.html, "derived from samples"));
    CHECK(has(r.html, "no feature samples for 11 s"));
}

TEST_CASE("DEC-13 Show Report: size is bounded; huge inputs truncate deterministically") {
    ReportLimits lim;
    lim.maxEvents = 5000;
    lim.maxHtmlBytes = 256 * 1024;
    Session s;
    for (std::uint64_t i = 0; i < 30000; ++i)
        s.events.push_back(ev(1000 + i, static_cast<Ns>(i % 170), EventKind::Device, "device event " + std::to_string(i) + std::string(200, 'x')));
    ShowReportBuilder b(lim);
    b.addEvents(s.events).addSamples(s.samples);
    const auto r = b.build();
    REQUIRE(r.ok());
    CHECK(r.value().truncated);
    CHECK(r.value().html.size() <= lim.maxHtmlBytes);
    CHECK(has(r.value().html, "more row(s) not shown"));
    CHECK(countOf(r.value().eventsCsv, "\r\n") <= 5001);
    // chart is bounded independent of sample count
    ShowReportBuilder many;
    std::vector<FeatureSample> dense;
    for (int i = 0; i < 100000; ++i) dense.push_back(smp(i, 120.0 + (i % 7), 0, false, static_cast<float>(i % 100) / 100.f, 0, 1));
    many.addSamples(dense);
    const auto m = many.build();
    REQUIRE(m.ok());
    CHECK(m.value().html.size() < 200 * 1024);
    // table row cap on its own
    ReportLimits rows; rows.maxTableRows = 20;
    ShowReportBuilder capped(rows);
    capped.addEvents(s.events);
    const auto c = capped.build();
    REQUIRE(c.ok());
    CHECK(c.value().truncated);
}

TEST_CASE("DEC-13 Show Report: secrets that reach the report are removed even from raw (unsanitised) events") {
    Event e = ev(1, 5, EventKind::Broadcast, "publishing to rtmps://u:p@live.example.com/app/abcd1234efgh5678ijkl key=SuperSecret99");
    ShowReportBuilder b;
    b.addEvents({e});
    const auto r = b.build();
    REQUIRE(r.ok());
    for (const auto& text : {r.value().html, r.value().eventsCsv}) {
        CHECK_FALSE(has(text, "u:p@"));
        CHECK_FALSE(has(text, "abcd1234efgh5678ijkl"));
        CHECK_FALSE(has(text, "SuperSecret99"));
    }
}

TEST_CASE("DEC-13 Show Report: a session built from the real journal and sampler classes") {
    FakeClock clock(0, kWall0 * kNsPerMs);
    EventJournal journal(clock);
    FeatureHistory history;
    FeatureSnapshot snap;
    snap.tempo.bpm = 124.0; snap.tempo.source = ValueSource::Estimated;
    snap.key.tonic = 2; snap.key.minor = false; snap.key.source = ValueSource::Estimated;
    journal.addMarker("Opener");
    for (int i = 0; i < 600; ++i) {                       // 60 s at 10 Hz of snapshots
        clock.advance(msToNs(100));
        snap.energyArc = static_cast<float>(i) / 600.f;
        snap.arcPhase = i > 400 && i < 450 ? 2 : 0;
        history.offer(clock.nowMonotonic(), snap, 1);
    }
    ShowReportBuilder b;
    b.addEvents(journal.snapshot()).addSamples(history.samples());
    const auto r = b.build();
    REQUIRE(r.ok());
    CHECK(history.size() == 60);
    REQUIRE(r.value().setlist.size() == 1);
    CHECK(r.value().setlist[0].label == "Opener");
    CHECK(r.value().setlist[0].medianBpm == doctest::Approx(124.0));
    CHECK(keyName(r.value().setlist[0].keyTonic, r.value().setlist[0].keyMinor) == "D major");
    CHECK(r.value().drops.size() == 1);
}
