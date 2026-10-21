// Replaces global operator new/delete in this test binary so tests can prove "zero allocations in render()".
#include <cstdlib>
#include <new>
#include "test_util.hpp"

namespace alloc_probe {
std::size_t count = 0;
bool armed = false;
}  // namespace alloc_probe

void* operator new(std::size_t n) {
    if (alloc_probe::armed) ++alloc_probe::count;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    if (alloc_probe::armed) ++alloc_probe::count;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
