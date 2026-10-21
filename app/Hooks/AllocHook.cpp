// Test hook, compiled into the STANDALONE executable only (never the CLAP module, which must not replace the
// host's allocator). Counts global operator new calls made while the calling thread has counting switched on.
// Used by the `shell: callback no alloc` self-test (REQ-P05). Overhead when off: one thread_local load.
#include <atomic>
#include <cstdlib>
#include <new>

namespace {
thread_local bool tlCounting = false;
std::atomic<unsigned long long> gCount{0};
}  // namespace

namespace ss::app {
// Registered into shared code at static-init time; a null pointer there means "no hook in this binary".
struct AllocHookApi { void (*setCounting)(bool); unsigned long long (*count)(); };
extern AllocHookApi* g_allocHook;
}  // namespace ss::app

namespace {
void setCounting(bool on) { tlCounting = on; }
unsigned long long count() { return gCount.load(std::memory_order_relaxed); }
ss::app::AllocHookApi api{&setCounting, &count};
struct Registrar { Registrar() { ss::app::g_allocHook = &api; } } registrar;
}  // namespace

void* operator new(std::size_t n) {
    if (tlCounting) gCount.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (tlCounting) gCount.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
