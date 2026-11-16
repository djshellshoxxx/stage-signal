#include "App/UiModel.h"
#include <algorithm>

namespace ss::app {

UiModel::UiModel() : outcomes_(kOutcomeQueue) {}

UiModel::Ticket UiModel::submitAction(ss::Action a) {
    Ticket t;
    if (a.correlationId == 0) a.correlationId = nextCorrelation_.fetch_add(1);
    t.correlationId = a.correlationId;
    t.result = inbox_.submit(a);
    return t;
}

bool UiModel::postCommand(AppCommand c, std::uint64_t* correlationOut) {
    if (c.correlationId == 0) c.correlationId = nextCorrelation_.fetch_add(1);
    if (correlationOut) *correlationOut = c.correlationId;
    std::lock_guard<std::mutex> g(cmdMutex_);
    if (commands_.size() >= kCommandQueue) return false;
    commands_.push_back(std::move(c));
    return true;
}

bool UiModel::popCommand(AppCommand& out) {
    std::lock_guard<std::mutex> g(cmdMutex_);
    if (commands_.empty()) return false;
    out = std::move(commands_.front());
    commands_.pop_front();
    return true;
}

void UiModel::postCommandOutcome(const CommandOutcome& o) {
    std::lock_guard<std::mutex> g(cmdOutMutex_);
    if (commandOutcomes_.size() >= kOutcomeQueue) commandOutcomes_.pop_front();
    commandOutcomes_.push_back(o);
}

std::shared_ptr<const SharedFrame> UiModel::latestFrame(OutputId id) const {
    return frameSource_ ? frameSource_(id) : nullptr;
}

void UiModel::addListener(UiModelListener* l) {
    if (l && std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end()) listeners_.push_back(l);
}

void UiModel::removeListener(UiModelListener* l) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
}

void UiModel::poll() {
    if (pollHook_) pollHook_();
    ActionOutcome ao;
    while (outcomes_.pop(ao)) {
        auto copy = listeners_;     // a listener may remove itself while handling
        for (auto* l : copy) l->uiActionOutcome(ao);
    }
    std::deque<CommandOutcome> co;
    { std::lock_guard<std::mutex> g(cmdOutMutex_); co.swap(commandOutcomes_); }
    for (const auto& o : co) {
        auto copy = listeners_;
        for (auto* l : copy) l->uiCommandOutcome(o);
    }
    const auto v = snap_.version();
    if (v != notifiedVersion_) {
        notifiedVersion_ = v;
        if (auto s = snap_.get()) {
            auto copy = listeners_;
            for (auto* l : copy) l->uiSnapshotChanged(*s);
        }
    }
}

}  // namespace ss::app
