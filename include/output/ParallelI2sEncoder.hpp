#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

// Canonical, chronological samples for eight parallel lanes at 3.2 MHz.
// Transport-specific DMA byte ordering belongs to the hardware backend.
namespace ParallelI2sEncoder
{
    static const std::size_t LaneCount = 8;
    static const std::size_t SamplesPerByte = 32;
    static const std::uint32_t MinimumResetMicroseconds = 350;

    struct Lane
    {
        const std::uint8_t* data;
        std::size_t size;
        std::uint32_t resetMicroseconds;

        Lane(const std::uint8_t* bytes = nullptr, std::size_t length = 0,
             std::uint32_t reset = 0)
            : data(bytes), size(length), resetMicroseconds(reset) {}
    };

    struct Layout
    {
        std::size_t payloadBytes;
        std::size_t resetBytes;
        std::size_t totalBytes;

        Layout() : payloadBytes(0), resetBytes(0), totalBytes(0) {}
    };

    inline bool GetLayout(const Lane (&lanes)[LaneCount], Layout& result)
    {
        result = Layout();
        std::size_t longest = 0;
        std::uint32_t resetMicroseconds = MinimumResetMicroseconds;
        for (std::size_t lane = 0; lane < LaneCount; ++lane)
        {
            if (lanes[lane].size && !lanes[lane].data) { return false; }
            if (lanes[lane].size > longest) { longest = lanes[lane].size; }
            if (lanes[lane].resetMicroseconds > resetMicroseconds)
            {
                resetMicroseconds = lanes[lane].resetMicroseconds;
            }
        }

        const std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
        if (longest > maximum / SamplesPerByte) { return false; }
        const std::size_t payload = longest * SamplesPerByte;

        // There are 16 samples per 5 microseconds. Round up both the required
        // low time and the final DMA length, without overflowing 32-bit math.
        const std::uint64_t resetSamples =
            (std::uint64_t(resetMicroseconds) * 16 + 4) / 5;
        const std::uint64_t alignedReset = (resetSamples + 3) & ~std::uint64_t(3);
        if (alignedReset > maximum) { return false; }
        const std::size_t reset = static_cast<std::size_t>(alignedReset);
        if (payload > maximum - reset) { return false; }

        result.payloadBytes = payload;
        result.resetBytes = reset;
        result.totalBytes = payload + reset;
        return true;
    }

    // The destination must not overlap any input lane. All argument/size
    // validation completes before the first write; failure leaves it untouched.
    inline bool Encode(const Lane (&lanes)[LaneCount], std::uint8_t* destination,
                       std::size_t capacity, std::size_t& written)
    {
        written = 0;
        Layout layout;
        if (!GetLayout(lanes, layout) || capacity < layout.totalBytes ||
            (layout.totalBytes && !destination))
        {
            return false;
        }

        const std::size_t longest = layout.payloadBytes / SamplesPerByte;
        std::size_t output = 0;
        for (std::size_t index = 0; index < longest; ++index)
        {
            std::uint8_t active = 0;
            std::uint8_t values[LaneCount] = {};
            for (std::size_t lane = 0; lane < LaneCount; ++lane)
            {
                if (index < lanes[lane].size)
                {
                    active |= std::uint8_t(1U << lane);
                    values[lane] = lanes[lane].data[index];
                }
            }

            for (std::uint8_t bit = 0x80; bit; bit >>= 1)
            {
                std::uint8_t ones = 0;
                for (std::size_t lane = 0; lane < LaneCount; ++lane)
                {
                    if (values[lane] & bit) { ones |= std::uint8_t(1U << lane); }
                }
                // A zero bit is 1000; a one bit is 1110. A finished lane
                // stays low throughout every later symbol, without extra bits.
                destination[output++] = active;
                destination[output++] = ones;
                destination[output++] = ones;
                destination[output++] = 0;
            }
        }
        if (layout.resetBytes)
        {
            std::memset(destination + output, 0, layout.resetBytes);
        }
        written = layout.totalBytes;
        return true;
    }
}
