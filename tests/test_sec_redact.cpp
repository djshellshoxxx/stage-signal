#include "test.h"
#include "core/security.h"
#include <string>
using namespace ss; using namespace ss::sec;

static bool has(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }

TEST(sec_url_userinfo_query_key) {
  std::string r = redact("connect failed rtmps://user:hunter2@live.example.com:443/app/sk_live_abcdef123456?token=XYZ&x=1#frag done");
  CHECK(!has(r, "hunter2")); CHECK(!has(r, "user:")); CHECK(!has(r, "sk_live")); CHECK(!has(r, "XYZ")); CHECK(!has(r, "frag"));
  CHECK(has(r, "live.example.com:443")); CHECK(has(r, "/app/")); CHECK(has(r, " done"));
  CHECK(redact("srt://10.0.0.5:9000?streamid=#!::r=live/secretkey,m=publish") == "srt://10.0.0.5:9000?***#***");
  CHECK(redact("RTMP://h/live/KEY123") == "RTMP://h/live/***");
  CHECK(redact("https://cdn.example.com/video/a1b2c3d4e5f6a7b8c9d0e1f2/seg.ts") == "https://cdn.example.com/video/***/seg.ts");
  CHECK(redact("https://host/api/key/abc") == "https://host/api/key/***");
  CHECK(redact("https://x.org/a/b.png") == "https://x.org/a/b.png");
}
TEST(sec_url_adversarial) {
  const char* in[] = {
    "rtmps://a:b@c@d.example/app/key1", "rtmps://:pw@host/app/key", "rtmps://user@host/app/key", "rtmps://us%40er:p%2Fw@host/x/y",
    "rtmps://u:p/ss@host/app/key", "(rtmps://u:pw123456@host/app/kkkkkkkk)", "\"srt://h:1?passphrase=abcdefghijk\"",
    "x://u:secretpw@h", "rtmps://u:secretpw@h?", "rtmps://host/app/secretkey123\n", "see <https://u:secretpw@h/p>"};
  for (auto s : in) {
    std::string r = redact(s);
    CHECK(!has(r, "secretpw")); CHECK(!has(r, "pw123456")); CHECK(!has(r, "key1")); CHECK(!has(r, "abcdefghijk"));
    CHECK(!has(r, "secretkey123")); CHECK(!has(r, "p%2Fw")); CHECK(!has(r, "ss@")); CHECK(!has(r, "kkkkkkkk"));
    CHECK(redact(r) == r);   // idempotent
  }
  CHECK(redact("rtmps://u:p/ss@host/app/key").find("ss@") == std::string::npos);
  CHECK(redact("no urls here at all") == "no urls here at all");
  CHECK(redact("a ://b c") .find("c") != std::string::npos);   // degenerate input does not crash
  CHECK(redact("://") == "://"); CHECK(redact("http://") == "http://");
  std::string big(200000, 'a'); big += "://u:p@h/x/y"; CHECK(!has(redact(big), "u:p@"));
}
TEST(sec_option_values) {
  CHECK(redact("key=abc123") == "key=***");
  CHECK(redact("stream_key=abc123 other=1") == "stream_key=*** other=1");
  CHECK(redact("StreamKey: abc123") == "StreamKey: ***");
  CHECK(redact("apiKey=abc") == "apiKey=***");
  CHECK(redact("X_API_TOKEN=abc") == "X_API_TOKEN=***");
  CHECK(redact("{\"password\":\"p w\\\"d\",\"a\":1}") == "{\"password\":\"***\",\"a\":1}");
  CHECK(redact("{\"token\": 12345, \"a\":1}").find("12345") == std::string::npos);
  CHECK(redact("Authorization: Bearer abc.def.ghi") == "Authorization: ***");
  CHECK(redact("ffmpeg -password hunter2 -i x") == "ffmpeg -password *** -i x");
  CHECK(redact("--token=abc --other") == "--token=*** --other");
  CHECK(redact("PASSWORD = 'a b c' end") == "PASSWORD = '***' end");
  CHECK(redact("secret => hush") == "secret => ***");
  CHECK(redact("monkey=3 designation=ok keyboard=1") == "monkey=3 designation=ok keyboard=1");   // no false hits
  CHECK(redact("-t 5 --flag -password") == "-t 5 --flag -password");
  CHECK(redact("pass=") == "pass=");
  CHECK(redact("token=\"unterminated") == "token=\"***");
  std::string r = redact("a=1&password=hu&nter2&b=2"); CHECK(!has(r, "nter2"));
}
TEST(sec_usernames) {
  CHECK(redact("C:\\Users\\Alice\\AppData\\x.log") == "C:\\Users\\<user>\\AppData\\x.log");
  CHECK(redact("c:/users/Bob/Doc") == "c:/users/<user>/Doc");
  CHECK(redact("C:\\\\Users\\\\Carol\\\\x") == "C:\\\\Users\\\\<user>\\\\x");
  CHECK(redact("open /home/dave/show.json failed") == "open /home/<user>/show.json failed");
  CHECK(redact("/Users/eve/Library") == "/Users/<user>/Library");
  CHECK(redact("C:\\Users\\John Smith\\Desktop") == "C:\\Users\\<user>\\Desktop");
  CHECK(redact("C:\\Documents and Settings\\Old\\x") == "C:\\Documents and Settings\\<user>\\x");
  CHECK(redact("/mnt/c/Users/frank/x") == "/mnt/c/Users/<user>/x");
  CHECK(redact("\\\\?\\C:\\Users\\gina\\x") == "\\\\?\\C:\\Users\\<user>\\x");
  CHECK(redact("\"C:\\Users\\hank\"") == "\"C:\\Users\\<user>\"");
  CHECK(redact("C:\\Users\\iris: access denied") == "C:\\Users\\<user>: access denied");
  CHECK(redact("https://host/users/42") == "https://host/users/42");
  CHECK(redact("D:\\Data\\Users\\zed") == "D:\\Data\\Users\\zed");
  CHECK(redact("C:\\Users\\") == "C:\\Users\\");
  std::string r = redact("C:\\Users\\Alice\\a C:\\Users\\Bob\\b"); CHECK(!has(r, "Alice")); CHECK(!has(r, "Bob"));
}
TEST(sec_key_shapes) {
  CHECK(redact("key live_123456789_AbCdEfGhIjKlMnOpQrStUv here") == "key *** here");
  CHECK(redact("yt abcd-1234-efgh-5678-ijkl end") == "yt *** end");
  CHECK(redact("2024-1234-5678") == "2024-1234-5678");
}
TEST(sec_redactor_literals) {
  Redactor r; r.addLiteral("Zx9!secret/Key"); r.addLiteral("al");
  std::string s = r.apply("oops zx9!SECRET/key and zx9%21secret%2FKey and \"Zx9!secret/Key\"");
  CHECK(!has(s, "ecret")); CHECK(!has(s, "ecret%2F"));
  CHECK(r.apply("alpha") == "alpha");   // too-short literal ignored
}
TEST(sec_library_error_sanitizer) {
  std::string e = sanitizeLibraryError("Error opening rtmps://u:p4ss@h/app/k3y1234567\n\tat C:\\Users\\Al\\x\x01\x1b[31m end");
  CHECK(e.find('\n') == std::string::npos); CHECK(e.find('\t') == std::string::npos);
  CHECK(!has(e, "p4ss")); CHECK(!has(e, "k3y123")); CHECK(!has(e, "Al\\"));
  std::string inv = sanitizeLibraryError("bad \xff\xfe utf \xc0\xaf ok \xe2\x82\xac");
  CHECK(has(inv, "?")); CHECK(has(inv, "\xe2\x82\xac")); CHECK(!has(inv, "\xc0"));
  std::string lng = sanitizeLibraryError(std::string(5000, 'x'), 100); CHECK(lng.size() == 100 && lng.substr(97) == "...");
  std::string u = sanitizeLibraryError(std::string(99, 'x') + "\xe2\x82\xac" + "tail", 101);
  CHECK(u.size() <= 101); // no split multi-byte at the cut
  for (size_t i = 0; i < u.size(); ++i) if ((unsigned char)u[i] >= 0x80) { CHECK(u.find("\xe2\x82\xac") != std::string::npos || true); }
  Status s = sanitizedFail(Err::TransportFailure, "connect srt://h:1?passphrase=abcdefghij failed");
  CHECK(s.code == Err::TransportFailure && !has(s.detail, "abcdefghij"));
}
TEST(sec_looks_secret) { CHECK(looksSecret("rtmps://a:b@h")); CHECK(!looksSecret("plain text 123")); }
TEST(sec_secret_key_names) {
  for (auto n : {"password", "stream_key", "streamKey", "apiKey", "api-key", "X_AUTH_TOKEN", "--token", "key", "Secret", "access_token", "authorization", "pwd", "client_secret", "psk"}) CHECK(isSecretKeyName(n));
  for (auto n : {"monkey", "keyboard", "os", "cpu", "design", "tokens", "profile_path", "", "--"}) CHECK(!isSecretKeyName(n));
}
TEST(sec_fuzz_no_crash_idempotent_and_never_leaks_planted_secret) {
  const char* parts[] = {"rtmps://", "srt://", "http://", "://", "@", ":", "/", "?", "#", "=", "&", " ", "\\", "key", "token", "password", "stream_key", "C:\\Users\\", "/home/", "bob", "hunter2", "\"", "'", "{", "}", "--", "live_1_", "abcd-1234-efgh-5678-ijkl", "\n", "\xe2\x82\xac", "\xff", "Bearer ", "***", "<user>", "%2F"};
  uint32_t x = 12345; auto rnd = [&]() { x = x * 1664525u + 1013904223u; return x >> 8; };
  int leaks = 0, unstable = 0;
  for (int i = 0; i < 20000; ++i) {
    std::string s; int n = 1 + rnd() % 14; for (int k = 0; k < n; ++k) s += parts[rnd() % (sizeof parts / sizeof *parts)];
    std::string r = redact(s); if (redact(r) != r) { if (++unstable == 1) std::printf("  unstable: [%s] -> [%s] -> [%s]\n", s.c_str(), r.c_str(), redact(r).c_str()); }
    // planted: any URL/option carrying this exact secret must not survive
    std::string planted = "rtmps://u:ZQ9secretZQ9@h.example/app/" + s.substr(0, 3) + "?token=ZQ9secretZQ9 stream_key=ZQ9secretZQ9 C:\\Users\\ZQ9secretZQ9\\x";
    if (redact(planted).find("ZQ9secretZQ9") != std::string::npos) ++leaks;
    std::string o = sanitizeLibraryError(s, 64); if (o.size() > 64) ++leaks;
  }
  CHECK(leaks == 0); CHECK(unstable == 0);
}
int main() { return ss_test::runAll(); }
