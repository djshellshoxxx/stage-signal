// Owner: engine-media-record. H-tests for media, recording and broadcast (app-adapters section 15).
// No hardware, no network. Process tests use a tiny fake ffmpeg written as a POSIX sh script into a temp folder
// (skipped, with a stated reason, where /bin/sh does not exist). Real ffmpeg is never required.
#include <chrono>
#include <fstream>
#include <thread>
#include <juce_graphics/juce_graphics.h>
#include "App/Media/MediaLayers.h"
#include "App/MediaWorker.h"
#include "Engine/Media/NativeProcess.h"
#include "Tests/SelfTest.h"
#include "ss/base/actions.hpp"
#include "ss/record/wav.hpp"

namespace ss::app {
namespace {

using Clock = std::chrono::steady_clock;
#define REQUIRE_T(c, msg) do { if (!(c)) return {false, msg}; } while (0)

struct TempDir {
    juce::File dir;
    TempDir() : dir(juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("ss_media_test", "", false)) { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
    std::string path() const { return dir.getFullPathName().toStdString(); }
};

bool havePosixShell() { return juce::File("/bin/sh").existsAsFile(); }

std::string writeFakeFfmpeg(const TempDir& t) {
    auto f = t.dir.getChildFile("ffmpeg");
    f.replaceWithText(
        "#!/bin/sh\n"
        "d=$(dirname \"$0\")\n"
        "case \"$*\" in\n"
        " *-version*) echo 'ffmpeg version 6.1 fake-test'; exit 0;;\n"
        " *-encoders*) printf 'Encoders:\\n ------\\n V....D h264_nvenc           NVIDIA H.264\\n V....D mjpeg               MJPEG\\n A....D aac                  AAC\\n'; exit 0;;\n"
        " *lavfi*) exit 0;;\n"
        "esac\n"
        "echo \"$@\" > \"$d/args.txt\"\n"
        "exec 4<&0\n"
        "cat <&3 > \"$d/audio.bin\" &\n"
        "cat <&4 > \"$d/video.bin\" &\n"
        "vp=$!\n"
        "n=0\n"
        "while kill -0 $vp 2>/dev/null; do\n"
        "  n=$((n+1000))\n"
        "  printf 'frame=1\\ntotal_size=%d\\nout_time_us=%d\\ndrop_frames=0\\nprogress=continue\\n' $n $((n*100))\n"
        "  sleep 0.02\n"
        "done\n"
        "wait\n", false, false, "\n");
    f.setExecutePermission(true);
    return f.getFullPathName().toStdString();
}

struct Rig {
    AdapterSet ad = makeFakeAdapterSet(false);
    MediaWorker w{ad};
    std::vector<float> blk = std::vector<float>(480 * 2);
    std::uint64_t idx = 0;
    std::shared_ptr<SharedFrame> frame = std::make_shared<SharedFrame>();
    explicit Rig(bool native) {
        if (native) ad.processes = makeNativeProcessSupervisor();
        frame->pixels = ss::PixelBuffer(320, 240);
        for (std::size_t i = 0; i < frame->pixels.rgba.size(); i += 4) { frame->pixels.rgba[i] = static_cast<std::uint8_t>(i); frame->pixels.rgba[i + 1] = 80; frame->pixels.rgba[i + 3] = 255; }
        for (std::size_t i = 0; i < blk.size(); ++i) blk[i] = 0.2f * static_cast<float>((i / 2) % 48) / 48.f;
        w.setFrameProvider([this](ss::OutputId) { return std::shared_ptr<const SharedFrame>(frame); });
    }
    ~Rig() { w.stop(); }
    void feed1() {
        AudioBlockView v{blk.data(), 480, 2, 48000, idx, ad.clock->nowMonotonic(), 0, 1};
        idx += 480;
        w.onProcessedBlock(v, v);
    }
    // Feeds real-time audio (10 ms blocks) until pred() or timeout.
    template <class P> bool until(P pred, int ms) {
        const auto t0 = Clock::now();
        while (Clock::now() - t0 < std::chrono::milliseconds(ms)) {
            feed1();
            MediaView m; RecordView r; BroadcastView b;
            w.fillView(m, r, b);
            if (pred(m, r, b)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
    void run(int ms) { until([](auto&, auto&, auto&) { return false; }, ms); }
    ss::ActionResult act(ss::ActionId id, ss::ActionSource src, double v = 0) {
        ss::Action a; a.actionId = ss::id(id); a.source = src; a.value = v;
        return w.handleAction(a);
    }
    MediaConfig cfg(const TempDir& t, const std::string& ff) {
        MediaConfig c; c.outputDir = t.path(); c.ffmpegPath = ff; c.showName = "test"; c.recordFps = 20;
        c.broadcast.host = "live.example.com"; c.broadcast.path = "live"; c.broadcast.streamKey.handle = "ref:testkey";
        c.broadcast.video.width = 640; c.broadcast.video.height = 360; c.broadcast.video.fps = 15;
        return c;
    }
};

std::string readAll(const juce::File& f) { return f.loadFileAsString().toStdString(); }

SelfTestResult processRoundTrip() {
    if (!havePosixShell()) return {true, "skipped: no /bin/sh on this platform"};
    auto sup = makeNativeProcessSupervisor();
    sup->start();
    auto p = sup->create();
    REQUIRE_T(p->start({"/bin/sh", "-c", "cat; echo to-err >&2; exit 7"}).ok(), "start failed");
    REQUIRE_T(sup->liveChildren() == 1, "live child not counted");
    const std::string msg = "hello";
    REQUIRE_T(sup->writeStdin(*p, reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size()), "stdin write refused");
    p->requestStop();
    std::string out, err;
    const auto t0 = Clock::now();
    while (p->running() && Clock::now() - t0 < std::chrono::seconds(3)) { out += p->readProgress(); err += p->readStderr(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    out += p->readProgress(); err += p->readStderr();
    REQUIRE_T(!p->running(), "child did not exit");
    REQUIRE_T(out == "hello", "stdout not captured separately");
    REQUIRE_T(err == "to-err\n", "stderr not captured separately");
    REQUIRE_T(p->exitCode() == 7, "exit code lost");
    REQUIRE_T(sup->liveChildren() == 0, "child not released");
    auto q = sup->create();
    REQUIRE_T(!q->start({"/definitely/not/here"}).ok(), "missing program must fail to start");
    auto k = sup->create();
    REQUIRE_T(k->start({"/bin/sh", "-c", "sleep 30"}).ok(), "start sleeper");
    k->kill();
    const auto t1 = Clock::now();
    while (k->running() && Clock::now() - t1 < std::chrono::seconds(3)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE_T(!k->running() && k->exitCode() != 0, "kill did not end the child");
    return {true, "stdin, separate stdout/stderr, exit code, kill"};
}

SelfTestResult probeFake() {
    if (!havePosixShell()) return {true, "skipped: no /bin/sh on this platform"};
    TempDir t;
    auto sup = makeNativeProcessSupervisor();
    auto pr = sup->probeFfmpeg(writeFakeFfmpeg(t));
    REQUIRE_T(pr.found && pr.hasH264 && pr.hasAac && pr.h264Verified && pr.mjpegVerified && pr.verifiedEncoders.size() == 2,
              "fake ffmpeg not recognised: found=" + std::to_string(pr.found) + " h264=" + std::to_string(pr.hasH264) + " aac=" + std::to_string(pr.hasAac) + " v264=" + std::to_string(pr.h264Verified) + " vmj=" + std::to_string(pr.mjpegVerified) + " why=" + pr.reason);
    auto none = sup->probeFfmpeg(t.path() + "/nope");
    REQUIRE_T(!none.found && !none.reason.empty(), "missing ffmpeg must give a reason");
    return {true, "discovery and encoder verification"};
}

SelfTestResult imageAndText() {
    TempDir t;
    juce::Image img(juce::Image::ARGB, 40, 20, true);
    img.clear(img.getBounds(), juce::Colour::fromRGBA(200, 40, 10, 255));
    auto png = t.dir.getChildFile("a.png");
    { juce::FileOutputStream os(png); juce::PNGImageFormat().writeImageToStream(img, os); }
    auto dec = decodeImageFile(png.getFullPathName().toStdString());
    REQUIRE_T(dec.ok(), "png decode failed");
    const auto& pb = dec.value().pixels;
    REQUIRE_T(pb.width == 40 && pb.height == 20 && pb.rgba[0] == 200 && pb.rgba[3] == 255, "png pixels wrong");
    auto svg = t.dir.getChildFile("x.png");
    svg.replaceWithText("<svg xmlns='http://www.w3.org/2000/svg'><script>1</script></svg>");
    REQUIRE_T(!decodeImageFile(svg.getFullPathName().toStdString()).ok(), "active content must be refused");
    REQUIRE_T(!decodeImageFile(t.path() + "/missing.png").ok(), "missing file must fail");
    auto txt = rasterText("Doors at 8", 320, 80);
    REQUIRE_T(txt.ok() && txt.value().width == 320 && txt.value().height == 80, "text raster size");
    std::size_t lit = 0;
    for (std::size_t i = 3; i < txt.value().rgba.size(); i += 4) lit += txt.value().rgba[i] > 0;
    REQUIRE_T(lit > 0, "text raster is empty");
    REQUIRE_T(!rasterText(std::string("a\0b", 3), 100, 40).ok(), "NUL in text must be refused");

    MediaLayers layers;
    REQUIRE_T(layers.setImage(0, png.getFullPathName().toStdString(), ss::media::Tier::Standard).ok(), "layer import");
    ss::compose::FrameInputs in;
    layers.fillLayers(1, 0, in);
    REQUIRE_T(in.mediaCount == 1 && in.media[0].frame && in.media[0].frame->width == 40, "layers not offered to the compositor");
    layers.clearAll();
    ss::compose::FrameInputs none;
    layers.fillLayers(1, 0, none);
    REQUIRE_T(none.mediaCount == 0, "cleared layers still offered");
    return {true, "png decode, active content refused, text raster, compositor image layer"};
}

SelfTestResult idleIsDisarmed() {
    TempDir t;
    Rig r(false);
    REQUIRE_T(r.w.start().ok(), "start");
    r.run(60);
    MediaView m; RecordView rv; BroadcastView b;
    r.w.fillView(m, rv, b);
    REQUIRE_T(m.workerAvailable && rv.state == RecordUiState::Off && b.state == BroadcastUiState::Off, "must be idle at start");
    REQUIRE_T(!r.act(ss::ActionId::StartRecording, ss::ActionSource::System).accepted, "System source must not start recording");
    REQUIRE_T(!r.act(ss::ActionId::StartRecording, ss::ActionSource::Show).accepted, "Show source must not start recording");
    REQUIRE_T(!r.act(ss::ActionId::StartBroadcast, ss::ActionSource::Remote, 1).accepted, "Remote source must not start broadcast");
    r.run(30);
    r.w.fillView(m, rv, b);
    REQUIRE_T(rv.state == RecordUiState::Off && b.state == BroadcastUiState::Off, "refused actions changed state");
    REQUIRE_T(m.cameraState == static_cast<unsigned char>(CameraState::Unsupported), "camera must report Unsupported here");
    return {true, "idle, non-user sources refused, camera Unsupported"};
}

SelfTestResult recordWavOnly() {
    TempDir t;
    Rig r(false);                                   // fake supervisor: ffmpeg absent
    r.w.configure(r.cfg(t, ""));
    REQUIRE_T(r.w.start().ok(), "start");
    r.run(30);
    REQUIRE_T(r.act(ss::ActionId::StartRecording, ss::ActionSource::Ui).accepted, "Ui start refused");
    REQUIRE_T(r.until([](auto&, auto& rv, auto&) { return rv.state == RecordUiState::Recording; }, 3000), "never reached Recording");
    r.act(ss::ActionId::AddMarker, ss::ActionSource::Ui);
    r.run(250);
    REQUIRE_T(r.act(ss::ActionId::StopRecording, ss::ActionSource::Ui).accepted, "stop refused");
    REQUIRE_T(r.until([](auto&, auto& rv, auto&) { return rv.state == RecordUiState::Off && rv.note.find("Saved") == 0; }, 3000), "did not finish");
    MediaView m; RecordView rv; BroadcastView b;
    r.w.fillView(m, rv, b);
    REQUIRE_T(rv.bytesWritten > 48000 && rv.markers == 1 && !rv.videoOn, "record view numbers");
    REQUIRE_T(rv.note.find("ffmpeg") != std::string::npos, "missing ffmpeg must be stated");
    juce::Array<juce::File> wavs;
    t.dir.findChildFiles(wavs, juce::File::findFiles, false, "*.wav*");
    REQUIRE_T(wavs.size() >= 1, "no wav file");
    juce::MemoryBlock mb;
    wavs[0].loadFileAsData(mb);
    auto hdr = ss::record::parseWavHeader(std::string(static_cast<const char*>(mb.getData()), std::min<std::size_t>(mb.getSize(), 128)));
    REQUIRE_T(hdr.ok() && hdr.value().rateHz == 48000 && hdr.value().channels == 2 && hdr.value().bitsPerSample == 24, "wav header");
    juce::Array<juce::File> meta;
    t.dir.findChildFiles(meta, juce::File::findFiles, false, "*.json");
    REQUIRE_T(meta.size() >= 1, "no session metadata");
    return {true, "wav recorded, marker, ffmpeg-missing stated, metadata written"};
}

SelfTestResult recordNoAudio() {
    TempDir t;
    Rig r(false);
    r.w.configure(r.cfg(t, ""));
    r.w.start();
    r.act(ss::ActionId::StartRecording, ss::ActionSource::Keyboard);             // no audio block ever arrived
    const auto t0 = Clock::now();
    MediaView m; RecordView rv; BroadcastView b;
    while (Clock::now() - t0 < std::chrono::seconds(3)) {
        r.w.fillView(m, rv, b);
        if (rv.state == RecordUiState::Failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_T(rv.state == RecordUiState::Failed && !rv.note.empty(), "must fail with a cause when no audio arrives");
    return {true, "no audio: Failed with cause"};
}

SelfTestResult recordVideoFake() {
    if (!havePosixShell()) return {true, "skipped: no /bin/sh on this platform"};
    TempDir t;
    Rig r(true);
    r.w.configure(r.cfg(t, writeFakeFfmpeg(t)));
    r.w.start();
    r.run(40);
    r.act(ss::ActionId::StartRecording, ss::ActionSource::Ui);
    std::string why;
    REQUIRE_T(r.until([&](auto&, auto& rv, auto&) { why = rv.note; return rv.state == RecordUiState::Recording && rv.videoOn; }, 4000), "video recording not running: " + why);
    r.run(400);
    r.act(ss::ActionId::StopRecording, ss::ActionSource::Ui);
    REQUIRE_T(r.until([](auto&, auto& rv, auto&) { return rv.state == RecordUiState::Off; }, 5000), "did not finalise");
    const auto args = readAll(t.dir.getChildFile("args.txt"));
    REQUIRE_T(args.find("rawvideo") != std::string::npos && args.find("segment") != std::string::npos && args.find("pipe:3") != std::string::npos, "argv shape");
    const auto vbytes = t.dir.getChildFile("video.bin").getSize(), abytes = t.dir.getChildFile("audio.bin").getSize();
    REQUIRE_T(vbytes > 0 && vbytes % (320 * 240 * 4) == 0, "video frames not delivered whole");
    REQUIRE_T(abytes > 0 && abytes % 8 == 0, "audio not delivered");
    REQUIRE_T(r.ad.processes->liveChildren() == 0, "ffmpeg left running");
    return {true, "video via fake ffmpeg: frames and audio piped, finalised, no child left"};
}

SelfTestResult broadcastFake() {
    if (!havePosixShell()) return {true, "skipped: no /bin/sh on this platform"};
    TempDir t;
    Rig r(true);
    r.ad.secrets->put("secret:testkey", "SUPERSECRETKEY123");
    r.w.configure(r.cfg(t, writeFakeFfmpeg(t)));
    r.w.start();
    r.run(40);
    REQUIRE_T(!r.act(ss::ActionId::StartBroadcast, ss::ActionSource::System, 1).accepted, "System start must be refused");
    REQUIRE_T(r.act(ss::ActionId::StartBroadcast, ss::ActionSource::Ui, 0).accepted, "request refused");
    REQUIRE_T(r.until([](auto&, auto&, auto& b) { return b.awaitingConfirm && !b.confirmText.empty(); }, 3000), "no confirmation summary");
    MediaView m; RecordView rv; BroadcastView b;
    r.w.fillView(m, rv, b);
    REQUIRE_T(b.confirmText.find("SUPERSECRET") == std::string::npos && b.state == BroadcastUiState::Off, "summary must not leak the key; still off");
    REQUIRE_T(r.ad.processes->liveChildren() == 0, "nothing may run before confirmation");
    REQUIRE_T(r.act(ss::ActionId::StartBroadcast, ss::ActionSource::Ui, 1).accepted, "confirm refused");
    std::string why;
    REQUIRE_T(r.until([&](auto&, auto&, auto& bb) { why = bb.note + "/" + std::to_string(int(bb.state)); return bb.state == BroadcastUiState::Live; }, 6000), "never went Live: " + why);
    r.w.fillView(m, rv, b);
    REQUIRE_T(b.note.find("SUPERSECRET") == std::string::npos && b.encryption.find("RTMPS") != std::string::npos, "view leaks key or hides encryption");
    r.run(300);
    REQUIRE_T(r.act(ss::ActionId::StopBroadcast, ss::ActionSource::Ui).accepted, "stop refused");
    REQUIRE_T(r.until([](auto&, auto&, auto& bb) { return bb.state == BroadcastUiState::Off; }, 3000), "did not stop");
    REQUIRE_T(t.dir.getChildFile("video.bin").getSize() > 0 && t.dir.getChildFile("audio.bin").getSize() > 0, "no media reached ffmpeg");
    r.run(50);
    REQUIRE_T(r.ad.processes->liveChildren() == 0, "ffmpeg left running after stop");
    // A second request without a new confirmation must not go live.
    REQUIRE_T(r.act(ss::ActionId::StartBroadcast, ss::ActionSource::Ui, 1).accepted, "late confirm is queued");
    r.run(120);
    r.w.fillView(m, rv, b);
    REQUIRE_T(b.state != BroadcastUiState::Live && r.ad.processes->liveChildren() == 0, "confirm without a request must not start");
    return {true, "confirm flow, key never in view, live, stop, unconfirmed start ignored"};
}

SelfTestResult broadcastNoFfmpeg() {
    TempDir t;
    Rig r(false);
    r.w.configure(r.cfg(t, ""));
    r.w.start();
    r.run(40);
    r.act(ss::ActionId::StartBroadcast, ss::ActionSource::Ui, 0);
    r.until([](auto&, auto&, auto& b) { return b.awaitingConfirm || b.state == BroadcastUiState::Failed; }, 2000);
    r.act(ss::ActionId::StartBroadcast, ss::ActionSource::Ui, 1);
    MediaView m; RecordView rv; BroadcastView b;
    REQUIRE_T(r.until([](auto&, auto&, auto& bb) { return bb.state == BroadcastUiState::Failed && !bb.note.empty(); }, 2000), "must fail with a cause");
    r.w.fillView(m, rv, b);
    REQUIRE_T(!m.ffmpegFound && !m.ffmpegNote.empty(), "media view must say ffmpeg is missing");
    return {true, "no ffmpeg: broadcast Failed with cause"};
}

}  // namespace

void registerMediaRecordSelfTests(SelfTestRegistry& r) {
    r.add("media:process-roundtrip", processRoundTrip);
    r.add("media:ffmpeg-discovery", probeFake);
    r.add("media:image-and-text", imageAndText);
    r.add("media:idle-and-disarmed", idleIsDisarmed);
    r.add("record:wav-only", recordWavOnly);
    r.add("record:no-audio-fails", recordNoAudio);
    r.add("record:video-via-fake-ffmpeg", recordVideoFake);
    r.add("broadcast:confirm-and-live-via-fake-ffmpeg", broadcastFake);
    r.add("broadcast:ffmpeg-missing", broadcastNoFfmpeg);
}

}  // namespace ss::app
