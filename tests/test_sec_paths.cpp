#include "test.h"
#include "core/security.h"
#include <filesystem>
#include <fstream>
using namespace ss; using namespace ss::sec;
namespace fs = std::filesystem;

TEST(sec_path_accepts_normal) {
  const char* ok[] = {"show.json", "media/ab12cd.png", "media/sub dir/clip.mp4", "./media/x.png", "a/b/c/d.txt", "Ünï/ç.png"};
  for (auto p : ok) CHECK(validateRelPath(p).ok());
  std::string c, f; CHECK(canonicalizeRelPath("./media\\Sub/./X.PNG", c, f).ok()); CHECK(c == "media/Sub/X.PNG" && f == "media/sub/x.png");
}
TEST(sec_path_rejects_hostile) {
  const char* bad[] = {"", "/etc/passwd", "\\windows\\x", "C:\\x.png", "c:x.png", "C:/x", "\\\\server\\share\\x", "//server/share/x", "../x", "a/../x", "a/..\\x",
    "a/b/..", "..", "...", ".. /x", "a/ ../b", "a/.. /b", "a/b.", "a/b ", "a//b", "a/b/", "a:stream", "a.png:Zone.Identifier", "\\\\?\\C:\\x", "\\\\.\\pipe\\x",
    "a/%2e%2e/b", "a/%2E%2e%5Cb", "%2fetc/passwd", "a\x01" "b", "con", "CON.png", "a/nul.txt", "COM1", "lpt9.x", "a<b", "a|b", "a*b", "a?b", "a\"b",
    "\xff\xfe", "\xc0\xaf", "a\xef\xbc\x8e\xef\xbc\x8e/b", "a\xe2\x88\x95" "b", "a\xe2\x80\xae" "b.png", " lead", "http://x/y.png", "file:///etc/x"};
  for (auto p : bad) { std::string s(p); if (validateRelPath(s).ok()) std::printf("accepted: [%s]\n", p); CHECK(!validateRelPath(s).ok()); }
  CHECK(!validateRelPath(std::string("a\0b", 3)).ok());
  CHECK(!validateRelPath(std::string(300, 'a')).ok());
  CHECK(!validateRelPath(std::string(200, 'a') + "/x").ok());
  std::string deep; for (int i = 0; i < 40; ++i) deep += "d/"; CHECK(!validateRelPath(deep + "x").ok());
  RelPathPolicy pol; pol.allowedExt = {".png", ".json"};
  CHECK(validateRelPath("a.PNG", pol).ok()); CHECK(!validateRelPath("a.exe", pol).ok()); CHECK(!validateRelPath("png", pol).ok()); CHECK(!validateRelPath("a.png.exe", pol).ok());
}
TEST(sec_duplicate_canonical) {
  CanonicalPathSet s(3);
  CHECK(s.add("media/A.png").ok()); CHECK(!s.add("MEDIA/a.PNG").ok()); CHECK(!s.add("./media/a.png").ok()); CHECK(!s.add("media\\a.png").ok());
  CHECK(s.add("b").ok()); CHECK(s.add("c").ok()); Status st = s.add("d"); CHECK(st.code == Err::ResourceExhausted);
}
TEST(sec_local_content_never_network) {
  const char* bad[] = {"http://evil/x.png", "HTTPS://evil/x", " http://evil/x", "\thttp://evil", "rtmp://h/a", "srt://h:1", "ftp://h/x", "udp://1.2.3.4:5", "tcp://h",
    "concat:a.png|http://evil/x", "subfile,,start,0,end,10,,:x", "data:image/png;base64,AAAA", "file:///c:/x", "x/http://evil", "ffconcat:x", "//evil/x", "-i", "-f lavfi", "rtsp://c", "a|b.png", "javascript:alert(1)"};
  for (auto p : bad) CHECK(!checkLocalContentRef(p).ok());
  CHECK(checkLocalContentRef("media/clip.mp4").ok());
  CHECK(checkLocalContentRef("http://x").detail.find("network") != std::string::npos);
  CHECK(isNetworkScheme("RTMPS") && isNetworkScheme("srt") && !isNetworkScheme("file"));
  CHECK(urlScheme("  HTTP://a") == "http" && urlScheme("C:\\x").empty() && urlScheme("a:b").empty() && urlScheme("ab:b") == "ab");
}
TEST(sec_stream_scheme_allowlist) {
  CHECK(checkStreamUrlScheme("rtmps://h/a").ok()); CHECK(checkStreamUrlScheme("srt://h:1").ok());
  CHECK(!checkStreamUrlScheme("rtmp://h/a").ok()); CHECK(checkStreamUrlScheme("rtmp://h/a", false, true).ok());
  CHECK(!checkStreamUrlScheme("rtp://h:1").ok()); CHECK(checkStreamUrlScheme("rtp://h:1", true).ok());
  CHECK(!checkStreamUrlScheme("http://h").ok()); CHECK(!checkStreamUrlScheme("h:1").ok()); CHECK(!checkStreamUrlScheme("file:///x").ok());
  CHECK(localDecoderProtocolAllowlist().size() == 1 && localDecoderProtocolAllowlist()[0] == "file");
}
TEST(sec_loopback_endpoints) {
  CHECK(isLoopbackOrDiscardEndpoint("srt://127.0.0.1:9000")); CHECK(isLoopbackOrDiscardEndpoint("rtmp://localhost/live/x"));
  CHECK(isLoopbackOrDiscardEndpoint("srt://[::1]:9")); CHECK(isLoopbackOrDiscardEndpoint("discard://sink")); CHECK(isLoopbackOrDiscardEndpoint("127.0.0.2:5"));
  CHECK(!isLoopbackOrDiscardEndpoint("srt://127.0.0.1.evil.com:9")); CHECK(!isLoopbackOrDiscardEndpoint("srt://example.com"));
  CHECK(!isLoopbackOrDiscardEndpoint("srt://127.0.0.1@evil.com")); CHECK(!isLoopbackOrDiscardEndpoint("")); CHECK(!isLoopbackOrDiscardEndpoint("srt://10.0.0.1"));
  CHECK(!isLoopbackOrDiscardEndpoint("srt://127.0.0.999")); CHECK(!isLoopbackOrDiscardEndpoint("srt://127.1"));
}
TEST(sec_import_limits) {
  CHECK(checkCount("cues", 10000, 10000).ok()); CHECK(checkCount("cues", 10001, 10000).code == Err::ResourceExhausted);
  uint64_t total = 0; ImportLimits L;
  CHECK(checkDecompressed(total, 1000, 100, L).ok() && total == 1000);
  CHECK(!checkDecompressed(total, 1000000, 100, L).ok());          // ratio 10000
  CHECK(!checkDecompressed(total, 5, 0, L).ok());
  total = L.maxDecompressedBytes - 10; CHECK(!checkDecompressed(total, 11, 11, L).ok());
  total = 100; CHECK(!checkDecompressed(total, UINT64_MAX, UINT64_MAX, L).ok());   // overflow-safe
  CHECK(checkDecompressed(total, 0, 0, L).ok());
}
TEST(sec_symlink_escape) {
  fs::path root = fs::temp_directory_path() / "ss_sec_test_root"; std::error_code ec; fs::remove_all(root, ec);
  fs::create_directories(root / "media"); { std::ofstream(root / "media" / "ok.png") << "x"; }
  fs::path outside = fs::temp_directory_path() / "ss_sec_test_outside"; fs::create_directories(outside);
  fs::create_directory_symlink(outside, root / "link", ec);
  CHECK(checkNoLinkEscape(root.string(), "media/ok.png").ok());
  CHECK(checkNoLinkEscape(root.string(), "media/not-yet.png").ok());
  if (!ec) { CHECK(!checkNoLinkEscape(root.string(), "link/x.png").ok()); }
  CHECK(!checkNoLinkEscape(root.string(), "../x").ok());
  CHECK(!checkNoLinkEscape(root.string(), "media/ok.png", [](const std::string& p) { return p.size() >= 6 && p.compare(p.size() - 6, 6, "ok.png") == 0; }).ok());
  fs::remove_all(root, ec); fs::remove_all(outside, ec);
}
int main() { return ss_test::runAll(); }
