#include "test.h"
#include "core/types.h"
#include "core/queue.h"
using namespace ss;
TEST(queue_bounded_drops) { BoundedQueue<int> q(2); CHECK(q.push(1)); CHECK(q.push(2)); CHECK(!q.push(3)); CHECK(q.drops()==1); int v; CHECK(q.pop(v)&&v==1); }
TEST(queue_drop_oldest) { BoundedQueue<int> q(2); q.pushDropOldest(1); q.pushDropOldest(2); q.pushDropOldest(3); int v; q.pop(v); CHECK(v==2); }
TEST(fake_clock) { FakeClock c; c.advance(kNsPerSec); CHECK(c.nowNs()==kNsPerSec); CHECK(std::string(errName(Err::DiskFull))=="DiskFull"); }
int main() { return ss_test::runAll(); }
