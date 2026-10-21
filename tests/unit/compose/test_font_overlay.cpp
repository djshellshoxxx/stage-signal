#include <cstring>
#include "util.hpp"
using namespace tu;

TEST_CASE("5x7 font: every printable glyph exists, space is blank, letters are distinct") {
    for (char c = 0x21; c <= 0x7E; ++c) {
        bool any = false;
        for (int i = 0; i < kGlyphW; ++i) any = any || glyphColumns(c)[i] != 0;
        CAPTURE(int(c));
        CHECK(any);
        for (int i = 0; i < kGlyphW; ++i) CHECK((glyphColumns(c)[i] & 0x80) == 0);  // only 7 rows
    }
    for (int i = 0; i < kGlyphW; ++i) CHECK(glyphColumns(' ')[i] == 0);
    for (char a = 'A'; a <= 'Z'; ++a)
        for (char b = char(a + 1); b <= 'Z'; ++b) CHECK(std::memcmp(glyphColumns(a), glyphColumns(b), 5) != 0);
    for (char a = 'a'; a <= 'z'; ++a)
        for (char b = char(a + 1); b <= 'z'; ++b) CHECK(std::memcmp(glyphColumns(a), glyphColumns(b), 5) != 0);
    for (char a = '0'; a <= '9'; ++a)
        for (char b = char(a + 1); b <= '9'; ++b) CHECK(std::memcmp(glyphColumns(a), glyphColumns(b), 5) != 0);
    // non-ASCII / control render as '?'
    CHECK(std::memcmp(glyphColumns(char(0xC3)), glyphColumns('?'), 5) == 0);
    CHECK(std::memcmp(glyphColumns('\n'), glyphColumns('?'), 5) == 0);
}

TEST_CASE("5x7 font: 'A' bitmap golden and text metrics") {
    // 'A' = 7E 11 11 11 7E, bit0 = top.
    // 'A' = 7E 11 11 11 7E (bit0 = top): column 0 has rows 1..6, column 1 has rows 0 and 4.
    CHECK(glyphPixel('A', 0, 1));
    CHECK(!glyphPixel('A', 0, 0));
    CHECK(glyphPixel('A', 1, 0));
    CHECK(glyphPixel('A', 1, 4));
    CHECK(!glyphPixel('A', 1, 3));
    CHECK(glyphPixel('A', 4, 6));
    CHECK(!glyphPixel('A', 5, 0));  // out of range
    CHECK(textWidth("", 3) == 0);
    CHECK(textWidth("AB", 1) == 11);
    CHECK(textWidth("AB", 2) == 22);
    CHECK(textHeight(3) == 21);
}

TEST_CASE("5x7 font: drawText paints exact pixels at scale 1 and 2 and clips at edges") {
    PixelBuffer b = solid(12, 9, Px{0, 0, 0, 255});
    drawText(b, 1, 1, "I", 1, Rgb{255, 255, 255});  // 'I' = 00 41 7F 41 00
    // column 2 of the glyph (0x7F) is fully on: x = 1 + 2 = 3, y = 1..7
    for (int y = 1; y <= 7; ++y) CHECK(at(b, 3, y) == Px{255, 255, 255, 255});
    CHECK(at(b, 3, 0) == Px{0, 0, 0, 255});
    CHECK(at(b, 3, 8) == Px{0, 0, 0, 255});
    CHECK(at(b, 2, 1) == Px{255, 255, 255, 255});   // column 1 (0x41): rows 0 and 6
    CHECK(at(b, 2, 4) == Px{0, 0, 0, 255});
    PixelBuffer c2 = solid(12, 16, Px{0, 0, 0, 255});
    drawText(c2, 0, 0, "I", 2, Rgb{255, 0, 0});
    CHECK(at(c2, 4, 0) == Px{255, 0, 0, 255});       // glyph col 2 -> x 4..5
    CHECK(at(c2, 5, 13) == Px{255, 0, 0, 255});
    drawText(c2, 10, 14, "AAAA", 2, Rgb{255, 255, 255});  // runs off the canvas: must not crash
    drawText(c2, -50, -50, "A", 3, Rgb{255, 255, 255});
}

TEST_CASE("overlay labels: Manual / Estimated / Tap markers, unknown values are never numbers") {
    char buf[64];
    ss::TempoState t;
    t.bpm = 128.0; t.source = ss::ValueSource::Manual;
    REQUIRE(formatTempoLabel(t, buf, sizeof buf));
    CHECK(std::string(buf) == "BPM 128 (Manual)");
    t.bpm = 127.5; t.source = ss::ValueSource::Estimated;
    REQUIRE(formatTempoLabel(t, buf, sizeof buf));
    CHECK(std::string(buf) == "BPM 127.5 (Estimated)");
    t.bpm = 120.04; t.source = ss::ValueSource::Tap;
    REQUIRE(formatTempoLabel(t, buf, sizeof buf));
    CHECK(std::string(buf) == "BPM 120 (Tap)");
    t.source = ss::ValueSource::Unknown;
    CHECK(!formatTempoLabel(t, buf, sizeof buf));
    t.source = ss::ValueSource::Manual; t.bpm = 0.0;
    CHECK(!formatTempoLabel(t, buf, sizeof buf));

    ss::KeyState k;
    k.tonic = 9; k.minor = true; k.source = ss::ValueSource::Estimated;
    REQUIRE(formatKeyLabel(k, buf, sizeof buf));
    CHECK(std::string(buf) == "Key Am (Estimated)");
    k.tonic = 1; k.minor = false; k.source = ss::ValueSource::Manual;
    REQUIRE(formatKeyLabel(k, buf, sizeof buf));
    CHECK(std::string(buf) == "Key C# (Manual)");
    k.tonic = -1;
    CHECK(!formatKeyLabel(k, buf, sizeof buf));
}

TEST_CASE("overlay redaction: path-like and secret-like diagnostics are recognised") {
    CHECK(looksSensitive("C:\\Users\\sheldon\\show.ssshow"));
    CHECK(looksSensitive("/home/sheldon/show"));
    CHECK(looksSensitive("file=/var/log/x"));
    CHECK(looksSensitive("stream key abc123"));
    CHECK(looksSensitive("rtmp://live.example/app"));
    CHECK(looksSensitive("Authorization Bearer xyz"));
    CHECK(looksSensitive("Password: hunter2"));
    CHECK(!looksSensitive("FPS 29.8 drops 0"));
    CHECK(!looksSensitive("audio 48000 Hz 2ch"));
    CHECK(!looksSensitive("cpu 12/100"));
}

namespace {
PixelBuffer renderWith(const CompositorConfig& cfg, const OverlayInfo* ov, std::uint32_t w = 160, std::uint32_t h = 96) {
    Compositor c;
    REQUIRE(c.prepare(w, h).ok());
    c.setConfig(cfg);
    const PixelBuffer scene = solid(w, h, Px{0, 0, 60, 255});
    FrameInputs in = sceneOnly(&scene);
    in.overlay = ov;
    ComposedFrame f = c.compose(in);
    REQUIRE(f.ok());
    PixelBuffer out = copyOf(f);
    c.release(f);
    return out;
}
OverlayInfo fullInfo() {
    OverlayInfo o;
    o.tempo.bpm = 128; o.tempo.source = ss::ValueSource::Manual;
    o.key.tonic = 9; o.key.minor = true; o.key.source = ss::ValueSource::Estimated;
    o.clipWarning = true;
    o.setBrandingText("MSR");
    o.addDiagnostic("FPS 30 drops 2");
    o.addDiagnostic("/home/x/secret.key");
    return o;
}
}  // namespace

TEST_CASE("REQ-D05: audience windows contain no diagnostics, clip warning, BPM/key or branding unless enabled") {
    const OverlayInfo info = fullInfo();
    CompositorConfig def;  // default policy: text only
    const PixelBuffer without = renderWith(def, nullptr);
    const PixelBuffer withInfo = renderWith(def, &info);
    CHECK(without.rgba == withInfo.rgba);

    CompositorConfig bpm = def; bpm.policy.showBpmKey = true;
    const PixelBuffer b1 = renderWith(bpm, &info);
    CHECK(b1.rgba != without.rgba);
    // enabling BPM/key does not enable diagnostics / clip / branding: diff stays in the top-left label area
    bool outside = false;
    for (int y = 0; y < 96; ++y)
        for (int x = 0; x < 160; ++x)
            if (at(b1, x, y) != at(without, x, y) && (x > 130 || y > 24)) outside = true;
    CHECK(!outside);

    CompositorConfig diag = def; diag.policy.showDiagnostics = true;
    const PixelBuffer d1 = renderWith(diag, &info);
    CHECK(d1.rgba != without.rgba);

    CompositorConfig brand = def; brand.policy.showBranding = true;
    CHECK(renderWith(brand, &info).rgba != without.rgba);
}

TEST_CASE("REQ-D05: diagnostics that look like paths or secrets are redacted even when enabled") {
    CompositorConfig diag; diag.policy.showDiagnostics = true;
    OverlayInfo a; a.addDiagnostic("/home/x/secret.key");
    OverlayInfo b; b.addDiagnostic("[redacted]");
    CHECK(renderWith(diag, &a, 96, 64).rgba == renderWith(diag, &b, 96, 64).rgba);
    OverlayInfo c; c.addDiagnostic("FPS 30");
    CHECK(renderWith(diag, &c, 96, 64).rgba != renderWith(diag, &b, 96, 64).rgba);
}

TEST_CASE("overlay: BPM label paints the expected glyph pixel and an unknown tempo paints nothing") {
    CompositorConfig cfg; cfg.policy.showBpmKey = true;
    OverlayInfo o;
    o.tempo.bpm = 128; o.tempo.source = ss::ValueSource::Manual;
    const PixelBuffer withTempo = renderWith(cfg, &o);
    // label at (m,m) = (2,2) scale 1; glyph origin at (3,3); 'B' column 0 (0x7F) -> white pixels x=3, y=3..9
    for (int y = 3; y <= 9; ++y) CHECK(at(withTempo, 3, y) == Px{255, 255, 255, 255});
    CHECK(at(withTempo, 2, 2).b < 60);  // translucent black backing darkens the scene
    OverlayInfo none;
    const PixelBuffer without = renderWith(cfg, &none);
    CHECK(allEqual(without, Px{0, 0, 60, 255}));
}
