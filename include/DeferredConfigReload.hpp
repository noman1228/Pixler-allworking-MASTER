#pragma once

#include <atomic>
#include <stdint.h>

// Schedulers may run on another core. Only the main loop consumes requests.
// Keep a generation so a save arriving during a reload remains pending.
class DeferredConfigReload
{
public:
    void Schedule(uint32_t nowMs)
    {
        ScheduledAt.store(nowMs, std::memory_order_relaxed);
        Generation.fetch_add(1, std::memory_order_release);
    }

    bool IsDue(uint32_t nowMs, uint32_t delayMs) const
    {
        return Generation.load(std::memory_order_acquire) != AppliedGeneration &&
            uint32_t(nowMs - ScheduledAt.load(std::memory_order_relaxed)) >= delayMs;
    }

    bool TakeIfDue(uint32_t nowMs, uint32_t delayMs)
    {
        const uint32_t generation = Generation.load(std::memory_order_acquire);
        if (generation == AppliedGeneration ||
            uint32_t(nowMs - ScheduledAt.load(std::memory_order_relaxed)) < delayMs)
        {
            return false;
        }
        AppliedGeneration = generation;
        return true;
    }

private:
    std::atomic<uint32_t> ScheduledAt{0};
    std::atomic<uint32_t> Generation{0};
    uint32_t AppliedGeneration = 0;
};
