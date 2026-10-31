#pragma once

#include <stdint.h>

// Shared by the runtime renderer and the host regression tests.
namespace TestModePatterns
{
    enum class Pattern { Red, Green, Blue, White, Rgb, Chase, Pixel, Count };
    struct Color { uint8_t red; uint8_t green; uint8_t blue; };

    inline Color PixelColor(Pattern pattern, uint32_t step, uint32_t pixelIndex,
                            uint32_t pixelCount, uint32_t segment,
                            uint8_t brightness, uint32_t selectedPixel)
    {
        if (!pixelCount || pixelIndex >= pixelCount) { return {0, 0, 0}; }
        switch (pattern)
        {
            case Pattern::Red:   return {brightness, 0, 0};
            case Pattern::Green: return {0, brightness, 0};
            case Pattern::Blue:  return {0, 0, brightness};
            case Pattern::White: return {brightness, brightness, brightness};
            case Pattern::Rgb:
                switch (step % 3)
                {
                    case 0: return {brightness, 0, 0};
                    case 1: return {0, brightness, 0};
                    default: return {0, 0, brightness};
                }
            case Pattern::Chase:
            {
                const uint32_t length = segment < pixelCount ? segment : pixelCount;
                const uint32_t start = step % pixelCount;
                const uint32_t distance = pixelIndex >= start ? pixelIndex - start : pixelCount - start + pixelIndex;
                return distance < length ? Color{brightness, brightness, brightness} : Color{0, 0, 0};
            }
            case Pattern::Pixel:
                return pixelIndex == selectedPixel && !(step & 1) ? Color{brightness, brightness, brightness} : Color{0, 0, 0};
            case Pattern::Count:
            {
                // The selected LED starts the marker. It shrinks at the end
                // without wrapping or moving until another LED is selected.
                const uint32_t length = segment < 5 ? segment : 5;
                return pixelIndex >= selectedPixel && pixelIndex - selectedPixel < length
                    ? Color{brightness, brightness, brightness} : Color{0, 0, 0};
            }
        }
        return {0, 0, 0};
    }
}
