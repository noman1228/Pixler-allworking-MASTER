#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
using std::min;

// Only the device storage is substituted. Both methods below are extracted
// from src/output/OutputPixel.cpp on every run.
struct c_OutputPixel
{
    uint32_t pixel_count = 10;
    uint32_t OutputBufferSize = 30;
    uint32_t NumIntensityBytesPerPixel = 3;
    uint32_t PixelGroupSize = 5;
    uint32_t zig_size = 4;
    bool MonsterPermanentRgb = false;
    uint32_t AdjustedBrightness = 256;
    uint8_t gamma_table[256]{};
    struct { uint8_t Array[4] = {1, 0, 2, 3}; } ColorOffsets;
    uint8_t* pOutputBuffer = nullptr;
    uint32_t GetTestPixelCount();
    void WriteTestPixel(uint32_t pixel, uint8_t red, uint8_t green, uint8_t blue);
};

#include "physical_methods.inc"

int main()
{
    std::array<uint8_t, 50> memory{};
    c_OutputPixel pixels;
    for (uint32_t i = 0; i < 256; ++i) pixels.gamma_table[i] = uint8_t(i);
    pixels.pOutputBuffer = memory.data() + 1;
    memory.front() = 201;
    memory[31] = 202;
    assert(pixels.GetTestPixelCount() == 10);
    pixels.WriteTestPixel(9, 11, 22, 33);
    for (size_t i = 1; i < 28; ++i) assert(memory[i] == 0);
    assert(memory[28] == 22 && memory[29] == 11 && memory[30] == 33);
    assert(memory.front() == 201 && memory[31] == 202);
    pixels.WriteTestPixel(10, 255, 255, 255);
    pixels.WriteTestPixel(UINT32_MAX, 255, 255, 255);
    assert(memory[31] == 202);

    // Count is restricted by allocated storage even if the configured count
    // is larger. Grouping and zig-zag do not replicate or remap this address.
    pixels.OutputBufferSize = 15;
    assert(pixels.GetTestPixelCount() == 5);
    pixels.WriteTestPixel(5, 99, 99, 99);
    assert(memory[16] == 0);
    pixels.OutputBufferSize = 30;
    for (uint32_t i = 0; i < 256; ++i) pixels.gamma_table[i] = uint8_t(i / 2);
    pixels.AdjustedBrightness = 128;
    pixels.WriteTestPixel(0, 200, 120, 80);
    assert(memory[1] == 30 && memory[2] == 50 && memory[3] == 20);
    pixels.MonsterPermanentRgb = true;
    pixels.WriteTestPixel(0, 200, 120, 80);
    assert(memory[1] == 120 && memory[2] == 200 && memory[3] == 80);

    // RGBW uses the dedicated white intensity without also lighting RGB.
    memory.fill(0);
    pixels.MonsterPermanentRgb = false;
    pixels.NumIntensityBytesPerPixel = 4;
    pixels.OutputBufferSize = 40;
    pixels.AdjustedBrightness = 256;
    for (uint32_t i = 0; i < 256; ++i) pixels.gamma_table[i] = uint8_t(i);
    memory[41] = 203;
    pixels.WriteTestPixel(9, 90, 90, 90);
    assert(memory[37] == 0 && memory[38] == 0 && memory[39] == 0 && memory[40] == 90);
    for (size_t i = 1; i < 37; ++i) assert(memory[i] == 0);
    assert(memory[41] == 203);
    pixels.WriteTestPixel(9, 90, 0, 0);
    assert(memory[37] == 0 && memory[38] == 90 && memory[39] == 0 && memory[40] == 0);
    pixels.pOutputBuffer = nullptr;
    pixels.WriteTestPixel(0, 255, 255, 255);
    std::cout << "Test Mode physical RGB/RGBW addressing regressions passed\n";
}
