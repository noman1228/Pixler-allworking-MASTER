#include <cassert>
#include <cstdint>
#include <iostream>
#include "output/TestModePatterns.hpp"

using TestModePatterns::Color;
using TestModePatterns::Pattern;
using TestModePatterns::PixelColor;

static bool same(Color color, uint8_t red, uint8_t green, uint8_t blue)
{
    return color.red == red && color.green == green && color.blue == blue;
}

int main()
{
    // Color tests must address every physical pixel, including the final LED.
    for (uint32_t pixel : {0u, 1u, 49u, 99u})
    {
        assert(same(PixelColor(Pattern::Red, 0, pixel, 100, 5, 64, 0), 64, 0, 0));
        assert(same(PixelColor(Pattern::Green, 0, pixel, 100, 5, 128, 0), 0, 128, 0));
        assert(same(PixelColor(Pattern::Blue, 0, pixel, 100, 5, 255, 0), 0, 0, 255));
        assert(same(PixelColor(Pattern::White, 0, pixel, 100, 5, 32, 0), 32, 32, 32));
    }
    for (uint32_t step = 0; step < 12; ++step)
    {
        const Color color = PixelColor(Pattern::Rgb, step, 99, 100, 5, 100, 0);
        assert(same(color, step % 3 == 0 ? 100 : 0,
                          step % 3 == 1 ? 100 : 0,
                          step % 3 == 2 ? 100 : 0));
    }

    // Chase must have a constant illuminated length and wrap across the end.
    for (uint32_t count : {1u, 2u, 10u, 100u})
    {
        for (uint32_t segment : {1u, 5u, 1000u})
        {
            for (uint32_t step = 0; step < count * 2; ++step)
            {
                uint32_t lit = 0;
                for (uint32_t pixel = 0; pixel < count; ++pixel)
                {
                    const Color color = PixelColor(Pattern::Chase, step, pixel, count, segment, 63, 0);
                    if (same(color, 63, 63, 63)) ++lit;
                    else assert(same(color, 0, 0, 0));
                }
                assert(lit == (segment < count ? segment : count));
            }
        }
    }
    assert(same(PixelColor(Pattern::Chase, 9, 9, 10, 3, 63, 0), 63, 63, 63));
    assert(same(PixelColor(Pattern::Chase, 9, 0, 10, 3, 63, 0), 63, 63, 63));
    assert(same(PixelColor(Pattern::Chase, 9, 1, 10, 3, 63, 0), 63, 63, 63));
    assert(same(PixelColor(Pattern::Chase, 9, 2, 10, 3, 63, 0), 0, 0, 0));

    // Blink exactly one LED, using the final physical LED as the selection.
    for (uint32_t step = 0; step < 6; ++step)
    {
        uint32_t lit = 0;
        for (uint32_t pixel = 0; pixel < 100; ++pixel)
        {
            const Color color = PixelColor(Pattern::Pixel, step, pixel, 100, 5, 80, 99);
            if (same(color, 80, 80, 80)) { ++lit; assert(pixel == 99); }
            else assert(same(color, 0, 0, 0));
        }
        assert(lit == (step % 2 == 0 ? 1u : 0u));
    }
    // The counting marker stays white, contiguous, and within the output.
    // Moving toward the final LED shrinks the group, without wrapping any
    // LEDs back to the start. The position always identifies the first lit LED.
    for (uint32_t count : {1u, 2u, 4u, 5u, 10u, 100u})
    {
        for (uint32_t segment = 1; segment <= 5; ++segment)
        {
            for (uint32_t selected = 0; selected < count; ++selected)
            {
                const uint32_t length = segment < count - selected ? segment : count - selected;
                for (uint32_t step : {0u, 1u, 2u, UINT32_MAX})
                {
                    uint32_t lit = 0;
                    for (uint32_t pixel = 0; pixel < count; ++pixel)
                    {
                        const Color color = PixelColor(Pattern::Count, step, pixel, count, segment, 71, selected);
                        const bool expected = pixel >= selected && pixel < selected + length;
                        assert(same(color, expected ? 71 : 0, expected ? 71 : 0, expected ? 71 : 0));
                        if (expected) ++lit;
                    }
                    assert(lit == length);
                }
            }
        }
    }
    for (Pattern pattern : {Pattern::Red, Pattern::Green, Pattern::Blue,
                            Pattern::White, Pattern::Rgb, Pattern::Chase, Pattern::Pixel, Pattern::Count})
    {
        assert(same(PixelColor(pattern, 0, 0, 0, 5, 80, 0), 0, 0, 0));
        assert(same(PixelColor(pattern, 0, 100, 100, 5, 80, 0), 0, 0, 0));
    }
    std::cout << "Test Mode pattern regressions passed\n";
}
