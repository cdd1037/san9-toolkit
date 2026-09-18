#pragma once

#include <chrono>

namespace san9 {

// A request stays pending until a successful present. New requests never postpone
// the deadline, so continuous input cannot starve presentation or lose its tail.
class PresentationSchedule {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kInterval = std::chrono::nanoseconds(16'666'667);

    void Request() { pending_ = true; }
    bool Pending() const { return pending_; }
    bool Due(Clock::time_point now) const { return pending_ && now >= next_; }
    void Complete(Clock::time_point now) {
        pending_ = false;
        next_ = now + kInterval;
    }
    void Retry(Clock::time_point now) { next_ = now + kInterval; }
    unsigned int DelayMilliseconds(Clock::time_point now) const {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(next_ - now);
        return remaining.count() > 0 ? static_cast<unsigned int>(remaining.count()) : 1;
    }

private:
    bool pending_ = false;
    Clock::time_point next_{};
};

} // namespace san9
