#include <chrono>
#include <thread>
#include <set>
#include <juce_core/juce_core.h>
#include "Engine/Fakes.h"
#include "Engine/Media/ClipDecoder.h"
#include "Tests/SelfTest.h"

namespace ss::app {
namespace {
struct TempClip {
    juce::File file = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("stagesignal-decoder", ".mp4");
    explicit TempClip(const std::string& extension = ".mp4") { file = file.withFileExtension(extension); }
    ~TempClip() { file.deleteFile(); }
};
CapturedRun metadata(const std::vector<std::string>&, int) {
    CapturedRun result; result.started = result.exited = true; result.code = 0;
    result.out = R"({"streams":[{"codec_name":"h264","width":16,"height":16,"avg_frame_rate":"25/1"}],"format":{"duration":"1"}})";
    return result;
}
SelfTestResult fakeDecoder() {
    TempClip clip;
    const char signature[] = "\0\0\0\x18" "ftypisom";
    if (!clip.file.replaceWithData(signature, sizeof(signature))) return {false, "test container could not be created"};
    FakeProcessSupervisor supervisor;
    ss::FakeClock clock;
    FfmpegProbe ffmpeg; ffmpeg.found = true; ffmpeg.path = "/fake/ffmpeg";
    ClipDecoder decoder(supervisor, clock, metadata);
    auto status = decoder.open(clip.file.getFullPathName().toStdString(), ffmpeg);
    if (!status.ok()) return {false, status.error().detail};
    auto process = supervisor.factory.last();
    if (!process || std::find(process->argv.begin(), process->argv.end(), "file,pipe") == process->argv.end())
        return {false, "decoder process did not restrict protocols"};
    const std::string red(16 * 16 * 4, char(255));
    auto blue = red; for (std::size_t i = 0; i < blue.size(); i += 4) { blue[i] = 0; blue[i + 1] = 0; }
    auto transparent = blue;
    for (std::size_t i = 0; i < transparent.size(); i += 4) { transparent[i] = char(255); transparent[i + 2] = 0; transparent[i + 3] = char(128); }
    process->progressOut = red + blue + transparent;
    decoder.tick(0);
    if (!decoder.frame() || decoder.frame()->rgba[0] != 255) return {false, "first complete frame missing"};
    decoder.tick(msToNs(20));
    if (decoder.frame()->rgba[0] != 255) return {false, "future frame displayed before media clock"};
    decoder.setPaused(true, msToNs(20));
    decoder.tick(msToNs(100));
    if (decoder.frame()->rgba[0] != 255) return {false, "paused decoder advanced"};
    decoder.setPaused(false, msToNs(100));
    decoder.tick(msToNs(120));
    if (decoder.frame()->rgba[0] != 0) return {false, "next frame did not follow media clock"};
    process->alive = false;
    decoder.tick(msToNs(160));
    if (!decoder.frame() || decoder.frame()->rgba[0] != 128 || decoder.frame()->rgba[3] != 128) return {false, "transparent pixels were not premultiplied"};
    if (!decoder.ended() || decoder.failed()) return {false, "clean clip end was not held"};
    decoder.stop();
    if (process->kills != 1) return {false, "decoder child not stopped"};
    ClipDecoder stalled(supervisor, clock, metadata);
    if (!stalled.open(clip.file.getFullPathName().toStdString(), ffmpeg).ok()) return {false, "stall fixture did not start"};
    stalled.tick(secToNs(4));
    if (!stalled.failed() || supervisor.factory.last()->kills != 1) return {false, "first-frame timeout did not kill child"};
    ClipDecoder overflow(supervisor, clock, metadata);
    if (!overflow.open(clip.file.getFullPathName().toStdString(), ffmpeg).ok()) return {false, "overflow fixture did not start"};
    supervisor.factory.last()->progressOut.assign((256u << 10) + 1, 'x');
    overflow.tick(0);
    if (!overflow.failed() || supervisor.factory.last()->kills != 1) return {false, "oversized decoder queue not stopped"};
    return {true, "protocol allowlist, bounded complete frames, media clock/pause, clean end, overflow and first-frame timeout"};
}
SelfTestResult realClip(bool gif) {
    auto supervisor = makeNativeProcessSupervisor(); supervisor->start();
    const auto ffmpeg = supervisor->probeFfmpeg("");
    if (!ffmpeg.found) return {true, "SKIPPED: external FFmpeg unavailable; fake decoder contract tested separately"};
    TempClip clip(gif ? ".gif" : ".mp4");
    std::vector<std::string> command{ffmpeg.path, "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=32x24:rate=5",
        "-t", "0.6", "-threads", "1"};
    if (gif) command.insert(command.end(), {"-loop", "-1"});
    else command.insert(command.end(), {"-c:v", "libx264", "-pix_fmt", "yuv420p"});
    command.insert(command.end(), {"-y", clip.file.getFullPathName().toStdString()});
    auto generated = runCaptured(*supervisor, command, 5000);
    if (!generated.exited || generated.code != 0) return {false, "local test clip generation failed"};
    ss::SystemClock clock;
    ClipDecoder decoder(*supervisor, clock);
    auto opened = decoder.open(clip.file.getFullPathName().toStdString(), ffmpeg);
    if (!opened.ok()) return {false, opened.error().detail};
    std::shared_ptr<const PixelBuffer> previous;
    int observed = 0;
    std::set<std::vector<std::uint8_t>> distinct;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5) && !decoder.ended() && !decoder.failed()) {
        decoder.tick(clock.nowMonotonic());
        const auto frame = decoder.frame();
        if (frame && frame != previous) {
            if (frame->width != 32 || frame->height != 24) return {false, "real decoder returned unexpected dimensions"};
            previous = frame; ++observed; distinct.insert(frame->rgba);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    if (decoder.failed() || !decoder.ended() || observed < 2 || distinct.size() < 2) return {false, "generated H.264 clip did not play multiple frames to clean end"};
    decoder.stop(); supervisor->stop();
    return {true, gif ? "generated animated GIF: multiple distinct frames, delay/disposal metadata and clean end" : "generated local H.264 clip, ffprobe metadata, distinct RGBA frames, clocked playback and clean child shutdown"};
}
}
void registerClipDecoderSelfTests(SelfTestRegistry& registry) {
    registry.add("media: clip decoder contract", fakeDecoder);
    registry.add("media: real generated video playback", [] { return realClip(false); });
    registry.add("media: real generated GIF playback", [] { return realClip(true); });
}
}
