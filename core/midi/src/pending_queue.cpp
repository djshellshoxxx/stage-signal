#include "ss/midi/pending_queue.hpp"
#include <algorithm>
#include "ss/base/actions.hpp"

namespace ss::midi {

PendingQueue::PendingQueue(std::size_t capacity) : cap_(capacity) { items_.reserve(capacity); }

bool PendingQueue::makeRoom(bool incomingContinuous) {
    if (items_.size() < cap_) return true;
    auto it = std::find_if(items_.begin(), items_.end(), [](const Pending& p) { return p.continuous; });
    if (it != items_.end()) {                     // stale continuous values go first
        items_.erase(it);
        ++c_.droppedContinuous;
        return true;
    }
    if (items_.empty()) { (incomingContinuous ? c_.droppedContinuous : c_.droppedDiscrete)++; return false; }
    items_.erase(items_.begin());                 // queue is all triggers: the oldest is the stalest
    ++c_.droppedDiscrete;
    return true;
}

void PendingQueue::pushContinuous(const Pending& p) {
    for (auto& e : items_) {
        if (e.continuous && e.mappingId == p.mappingId && e.portId == p.portId) {
            e.action.value = p.action.value;
            e.action.correlationId = p.action.correlationId;
            e.at = p.at;
            ++c_.coalesced;
            return;
        }
    }
    if (!makeRoom(true)) return;
    Pending q = p; q.continuous = true;
    items_.push_back(q);
}

void PendingQueue::pushDiscrete(const Pending& p) {
    if (!makeRoom(false)) return;
    Pending q = p; q.continuous = false;
    items_.push_back(q);
}

std::size_t PendingQueue::purgePort(std::uint32_t portId) {
    const std::size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Pending& p) { return p.portId == portId; }),
                 items_.end());
    const std::size_t n = before - items_.size();
    c_.purgedStale += n;
    return n;
}

void PendingQueue::drain(std::vector<Pending>& out) {
    out.insert(out.end(), items_.begin(), items_.end());
    items_.clear();
}

void PriorityLatch::set(const Action& a, std::uint32_t portId, Generation gen) {
    for (auto& s : items_) {
        if (s.action.actionId == a.actionId) { s.action = a; s.portId = portId; s.gen = gen; return; }
    }
    items_.push_back({a, portId, gen});
}

std::size_t PriorityLatch::purgePortUnsafe(std::uint32_t portId) {
    const std::size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Slot& s) {
        return s.portId == portId && s.action.actionId == id(ActionId::RestoreBlackout);
    }), items_.end());
    return before - items_.size();
}

void PriorityLatch::drain(std::vector<Action>& out) {
    std::sort(items_.begin(), items_.end(), [](const Slot& a, const Slot& b) {
        const bool ae = a.action.actionId == id(ActionId::EmergencyBlackout), be = b.action.actionId == id(ActionId::EmergencyBlackout);
        if (ae != be) return ae;
        return a.action.actionId < b.action.actionId;
    });
    for (const auto& s : items_) out.push_back(s.action);
    items_.clear();
}

}  // namespace ss::midi
