// Minimal test harness: TEST(name){ CHECK(cond); CHECK_NEAR(a,b,tol); } ; int main(){ return ss_test::runAll(); }
#pragma once
#include <cstdio>
#include <cmath>
#include <vector>
#include <functional>
#include <string>
namespace ss_test {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }
struct Reg { Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };
inline int runAll() {
  for (auto& c : cases()) { int before = failures(); c.fn(); std::printf("[%s] %s\n", failures() == before ? " ok " : "FAIL", c.name); }
  std::printf("%zu tests, %d failed checks\n", cases().size(), failures());
  return failures() ? 1 : 0;
}
}
#define SS_CAT2(a,b) a##b
#define SS_CAT(a,b) SS_CAT2(a,b)
#define TEST(name) static void SS_CAT(t_,name)(); static ss_test::Reg SS_CAT(r_,name)(#name, SS_CAT(t_,name)); static void SS_CAT(t_,name)()
#define CHECK(c) do { if (!(c)) { std::printf("  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); ++ss_test::failures(); } } while (0)
#define CHECK_NEAR(a,b,tol) do { double _a=(a), _b=(b); if (!(std::fabs(_a-_b) <= (tol))) { std::printf("  CHECK_NEAR failed %s:%d: %s=%g vs %s=%g tol %g\n", __FILE__, __LINE__, #a,_a,#b,_b,(double)(tol)); ++ss_test::failures(); } } while (0)
