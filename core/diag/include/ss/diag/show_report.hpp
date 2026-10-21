// StageSignal diag: Show Report (DEC-13). Turns a session's journal events and 1 Hz feature history into
//   (a) one self-contained HTML file: inline CSS, inline SVG energy/BPM timeline, setlist, scene usage, drops,
//       device/recording/broadcast events and discontinuities; and
//   (b) CSV exports for events and for the setlist.
// Guarantees: every dynamic string is HTML-escaped (or CSV-quoted with formula neutralisation); no scripts, no external
// resources (a CSP meta forbids them anyway); output depends only on the inputs (no clock, no locale, no randomness);
// size is bounded by ReportLimits (tables shrink deterministically, `truncated` says so).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"
#include "ss/diag/journal.hpp"
#include "ss/diag/sampler.hpp"

namespace ss::diag {

struct ReportLimits {
    std::size_t maxEvents = 20000;        // events consumed (extra ones are dropped, truncated=true)
    std::size_t maxSamples = 100000;      // feature samples consumed
    std::size_t maxTableRows = 500;       // rows per HTML table (halved until the page fits)
    std::size_t maxChartPoints = 600;     // buckets drawn per series
    std::size_t maxMarkers = 500;
    std::size_t maxHtmlBytes = 1536 * 1024;
    std::size_t maxCsvBytes = 8u * 1024 * 1024;
    Ns gapThresholdNs = 5 * kNsPerSec;    // feature-sample gap reported as a derived discontinuity
};

struct SceneName { std::uint32_t id = 0; std::string name; };

struct SetlistEntry {
    std::size_t index = 0;                // 1-based
    std::string label;
    Ns startNs = 0, endNs = 0;            // relative to the session start
    double medianBpm = 0.0;               // 0 = no valid tempo samples in the section
    int keyTonic = -1;
    bool keyMinor = false;
    float peakEnergy = 0.f;
    std::size_t sampleCount = 0;
    bool implicitSection = false;         // no markers existed: whole session as one section
};

struct SceneUsage {
    std::uint32_t id = 0;
    std::string name;
    std::size_t activations = 0;
    Ns totalNs = 0;
    int permille = 0;                     // share of the session, 0..1000
};

struct DropEntry { Ns atNs = 0; float energy = 0.f; double bpm = 0.0; std::uint32_t sceneId = 0; std::string sceneName; };

struct ReportSummary {
    Ns durationNs = 0;
    std::int64_t wallStartUnixMs = 0;     // 0 when unknown
    std::size_t events = 0, samples = 0, markers = 0, drops = 0, discontinuities = 0, scenesUsed = 0;
    double medianBpm = 0.0;
    int keyTonic = -1;
    bool keyMinor = false;
};

struct ShowReport {
    std::string html, eventsCsv, setlistCsv;
    ReportSummary summary;
    std::vector<SetlistEntry> setlist;
    std::vector<SceneUsage> sceneUsage;
    std::vector<DropEntry> drops;
    bool truncated = false;
};

class ShowReportBuilder {
public:
    explicit ShowReportBuilder(ReportLimits limits = ReportLimits{}) : limits_(limits) {}
    ShowReportBuilder& setTitle(const std::string& title) { title_ = title; return *this; }
    ShowReportBuilder& setAppVersion(const std::string& v) { appVersion_ = v; return *this; }
    ShowReportBuilder& setGeneratedAtWallMs(std::int64_t ms) { generatedAtMs_ = ms; return *this; }   // caller-supplied: keeps output deterministic
    ShowReportBuilder& setSceneNames(std::vector<SceneName> names) { sceneNames_ = std::move(names); return *this; }
    ShowReportBuilder& addEvents(const std::vector<Event>& events);
    ShowReportBuilder& addSamples(const std::vector<FeatureSample>& samples);

    Result<ShowReport> build() const;

private:
    ReportLimits limits_;
    std::string title_ = "StageSignal show", appVersion_;
    std::int64_t generatedAtMs_ = 0;
    std::vector<SceneName> sceneNames_;
    std::vector<Event> events_;
    std::vector<FeatureSample> samples_;
    bool inputTruncated_ = false;
};

}  // namespace ss::diag
