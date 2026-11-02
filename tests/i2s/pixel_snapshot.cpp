#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#define IRAM_ATTR
#define DEBUG_START
#define DEBUG_END
static unsigned checks = 0, snapshots = 0;
static void require(bool condition, const char* message)
{
    ++checks;
    if (!condition) { throw std::runtime_error(message); }
}

// Hardware and configuration are supplied at the seam; the wire serializer,
// its FSM transitions, frame counting, and snapshot loop are production code.
struct c_OutputCommon
{
    uint8_t* pOutputBuffer = nullptr;
    uint32_t OutputBufferSize = 0;
    unsigned frameReports = 0;
    uint8_t* GetBufferAddress() { return pOutputBuffer; }
    void ReportNewFrame() { ++frameReports; }
};

struct c_OutputPixel : c_OutputCommon
{
    bool MonsterPermanentRgb = false, InvertData = false;
    uint8_t MonsterRecord[6] = {}, MonsterRecordIndex = 6;
    uint32_t NumIntensityBytesPerPixel = 3, IntensityMultiplier = 1;
    uint8_t* NextPixelToSend = nullptr;
    uint32_t pixel_count = 0, SentPixelsCount = 0, PixelIntensityCurrentIndex = 0, PixelIntensityCurrentColor = 0;
    uint8_t* pFramePrependData = nullptr;
    uint32_t FramePrependDataSize = 0, FramePrependDataCurrentIndex = 0;
    uint8_t* pFrameAppendData = nullptr;
    uint32_t FrameAppendDataSize = 0, FrameAppendDataCurrentIndex = 0;
    uint8_t* PixelPrependData = nullptr;
    uint32_t PixelPrependDataSize = 0, PixelPrependDataCurrentIndex = 0;
    uint32_t PrependNullPixelCount = 0, PrependNullPixelCurrentCount = 0;
    uint32_t AppendNullPixelCount = 0, AppendNullPixelCurrentCount = 0;
    uint32_t GECEPixelId = 0, AdjustedBrightness = 256;
    uint8_t gamma_table[256] = {};
    uint32_t (c_OutputPixel::*FrameStateFuncPtr)() = &c_OutputPixel::FrameDone;

    c_OutputPixel()
    {
        for (unsigned value = 0; value < 256; ++value) { gamma_table[value] = uint8_t(value); }
    }
    void StartNewFrame();
    void SetStartingSendPixelState();
    uint32_t GetIntensityData();
    uint32_t FramePrependData();
    uint32_t PixelPrependNulls();
    uint32_t PixelSendPrependIntensity();
    uint32_t PixelSendIntensity();
    uint32_t PixelAppendNulls();
    uint32_t FrameAppendData();
    uint32_t FrameDone();
    bool ISR_GetNextIntensityToSend(uint32_t&);
    uint64_t GetWireFrameByteCount() const;
    bool SnapshotFrame(uint8_t*, size_t, size_t&);
#include "pixel_snapshot_inline.inc"
};

#include "pixel_snapshot_methods.inc"

static std::vector<uint8_t> reference(const std::vector<uint8_t>& source, unsigned width,
                                    unsigned leadingNulls, unsigned trailingNulls,
                                    const std::vector<uint8_t>& framePrefix,
                                    const std::vector<uint8_t>& frameSuffix,
                                    const std::vector<uint8_t>& pixelPrefix)
{
    std::vector<uint8_t> result = framePrefix;
    for (unsigned pixel = 0; pixel < leadingNulls; ++pixel)
    {
        result.insert(result.end(), pixelPrefix.begin(), pixelPrefix.end());
        result.insert(result.end(), width, 0);
    }
    for (size_t offset = 0; offset < source.size(); offset += width)
    {
        result.insert(result.end(), pixelPrefix.begin(), pixelPrefix.end());
        result.insert(result.end(), source.begin() + offset, source.begin() + std::min(source.size(), offset + width));
    }
    for (unsigned pixel = 0; pixel < trailingNulls; ++pixel)
    {
        result.insert(result.end(), pixelPrefix.begin(), pixelPrefix.end());
        result.insert(result.end(), width, 0);
    }
    result.insert(result.end(), frameSuffix.begin(), frameSuffix.end());
    return result;
}

static std::vector<uint8_t> monsterReference(const std::vector<uint8_t>& rgb)
{
    std::vector<uint8_t> result;
    for (size_t offset = 0; offset < rgb.size(); offset += 3)
    {
        uint8_t r = rgb[offset], g = offset + 1 < rgb.size() ? rgb[offset + 1] : 0;
        uint8_t b = offset + 2 < rgb.size() ? rgb[offset + 2] : 0;
        if (r == g && g == b) { result.insert(result.end(), {0, 0, 0, r, 0, 0}); }
        else { result.insert(result.end(), {r, g, b, 0, 0, 0}); }
    }
    return result;
}

static void verify(c_OutputPixel& pixel, const std::vector<uint8_t>& expected)
{
    require(pixel.GetWireFrameByteCount() == expected.size(), "Wire byte count differs from independent frame model");
    std::vector<uint8_t> guarded(expected.size() + 18, 0xA5);
    const unsigned reports = pixel.frameReports;
    for (unsigned attempt = 0; attempt < 3; ++attempt)
    {
        size_t written = 999;
        require(pixel.SnapshotFrame(guarded.data() + 9, expected.size(), written), "Valid snapshot rejected");
        require(written == expected.size(), "Snapshot reported the wrong byte count");
        require(std::equal(expected.begin(), expected.end(), guarded.begin() + 9), "Serialized bytes differ from expected wire stream");
        require(std::all_of(guarded.begin(), guarded.begin() + 9, [](uint8_t value) { return value == 0xA5; }) &&
                std::all_of(guarded.end() - 9, guarded.end(), [](uint8_t value) { return value == 0xA5; }),
                "Snapshot overwrote a destination canary");
        require(!pixel.ISR_MoreDataToSend(), "Snapshot left the serializer unfinished");
        ++snapshots;
    }
    require(pixel.frameReports == reports + 3, "Snapshot did not start exactly one serializer frame");
    const std::vector<uint8_t> before = guarded;
    size_t written = 999;
    require(!pixel.SnapshotFrame(guarded.data() + 9, expected.size() - 1, written) && written == 0,
            "Snapshot accepted an undersized destination");
    require(guarded == before && pixel.frameReports == reports + 3,
            "Rejected snapshot modified data or started a frame");
    require(!pixel.SnapshotFrame(nullptr, expected.size(), written) && written == 0,
            "Snapshot accepted a null destination");
    require(pixel.SnapshotFrame(guarded.data() + 9, expected.size() + 5, written) &&
            written == expected.size() && guarded == before,
            "Spare destination capacity changed the frame length or trailing bytes");
    ++snapshots;
}

int main()
{
    try
    {
        for (unsigned width : {3U, 4U})
        {
            for (unsigned length : {1U, 2U, 3U, 4U, 7U, 12U, 31U})
            {
                std::vector<uint8_t> source(length);
                for (unsigned index = 0; index < length; ++index) { source[index] = uint8_t(index * 41 + 17); }
                for (unsigned leadingNulls : {0U, 2U})
                for (unsigned trailingNulls : {0U, 3U})
                for (unsigned framing = 0; framing < 8; ++framing)
                {
                    std::vector<uint8_t> framePrefix = framing & 1 ? std::vector<uint8_t>{0xC1, 0xC2} : std::vector<uint8_t>{};
                    std::vector<uint8_t> frameSuffix = framing & 2 ? std::vector<uint8_t>{0xE1, 0xE2, 0xE3} : std::vector<uint8_t>{};
                    std::vector<uint8_t> pixelPrefix = framing & 4 ? std::vector<uint8_t>{0xA1, 0xA2} : std::vector<uint8_t>{};
                    c_OutputPixel pixel;
                    pixel.NumIntensityBytesPerPixel = width;
                    pixel.pOutputBuffer = source.data();
                    pixel.OutputBufferSize = length;
                    pixel.PrependNullPixelCount = leadingNulls;
                    pixel.AppendNullPixelCount = trailingNulls;
                    pixel.pFramePrependData = framePrefix.empty() ? nullptr : framePrefix.data();
                    pixel.FramePrependDataSize = uint32_t(framePrefix.size());
                    pixel.pFrameAppendData = frameSuffix.empty() ? nullptr : frameSuffix.data();
                    pixel.FrameAppendDataSize = uint32_t(frameSuffix.size());
                    pixel.PixelPrependData = pixelPrefix.empty() ? nullptr : pixelPrefix.data();
                    pixel.PixelPrependDataSize = uint32_t(pixelPrefix.size());
                    const auto expected = reference(source, width, leadingNulls, trailingNulls, framePrefix, frameSuffix, pixelPrefix);
                    const auto original = source;
                    verify(pixel, expected);
                    require(source == original, "Snapshot modified its source buffer");
                }
            }
        }

        std::vector<uint8_t> raw = {1, 2, 3, 64, 64, 64, 255, 0, 0, 0, 0, 0};
        c_OutputPixel monster;
        monster.MonsterPermanentRgb = true;
        monster.pOutputBuffer = raw.data();
        monster.OutputBufferSize = uint32_t(raw.size());
        verify(monster, monsterReference(raw));
        monster.PrependNullPixelCount = 1;
        monster.AppendNullPixelCount = 2;
        verify(monster, monsterReference(reference(raw, 3, 1, 2, {}, {}, {})));
        for (unsigned length : {1U, 2U, 4U, 7U})
        {
            c_OutputPixel partial;
            partial.MonsterPermanentRgb = true;
            partial.pOutputBuffer = raw.data();
            partial.OutputBufferSize = length;
            verify(partial, monsterReference(std::vector<uint8_t>(raw.begin(), raw.begin() + length)));
        }
        std::vector<uint8_t> monsterPrefix = {0x91}, monsterSuffix = {0xD1}, monsterPixelPrefix = {0x51};
        monster.pFramePrependData = monsterPrefix.data();
        monster.FramePrependDataSize = 1;
        monster.pFrameAppendData = monsterSuffix.data();
        monster.FrameAppendDataSize = 1;
        monster.PixelPrependData = monsterPixelPrefix.data();
        monster.PixelPrependDataSize = 1;
        verify(monster, monsterReference(reference(raw, 3, 1, 2, monsterPrefix, monsterSuffix, monsterPixelPrefix)));

        // Gamma/brightness are applied once to Monster records; ordinary RGB
        // and RGBW snapshots preserve the already-corrected raw output bytes.
        std::vector<uint8_t> correctedSource = {64, 128, 255, 200, 200, 200};
        c_OutputPixel corrected;
        corrected.MonsterPermanentRgb = true;
        corrected.pOutputBuffer = correctedSource.data();
        corrected.OutputBufferSize = uint32_t(correctedSource.size());
        corrected.AdjustedBrightness = 128;
        for (unsigned value = 0; value < 256; ++value) { corrected.gamma_table[value] = uint8_t(value * value / 255); }
        verify(corrected, {8, 32, 127, 0, 0, 0, 0, 0, 0, 78, 0, 0});
        c_OutputPixel ordinaryCorrected;
        ordinaryCorrected.pOutputBuffer = correctedSource.data();
        ordinaryCorrected.OutputBufferSize = uint32_t(correctedSource.size());
        ordinaryCorrected.AdjustedBrightness = 0;
        std::fill(std::begin(ordinaryCorrected.gamma_table), std::end(ordinaryCorrected.gamma_table), uint8_t(0));
        verify(ordinaryCorrected, correctedSource);

        c_OutputPixel inverted;
        inverted.InvertData = true;
        inverted.pOutputBuffer = raw.data();
        inverted.OutputBufferSize = uint32_t(raw.size());
        auto inverse = raw;
        for (auto& value : inverse) { value = uint8_t(~value); }
        verify(inverted, inverse);

        uint8_t source[3] = {1, 2, 3}, destination[32];
        for (unsigned malformed = 0; malformed < 4; ++malformed)
        {
            c_OutputPixel invalid;
            invalid.pOutputBuffer = source;
            invalid.OutputBufferSize = 3;
            if (malformed == 0) { invalid.pOutputBuffer = nullptr; }
            if (malformed == 1) { invalid.FramePrependDataSize = 1; }
            if (malformed == 2) { invalid.FrameAppendDataSize = 1; }
            if (malformed == 3) { invalid.PixelPrependDataSize = 1; }
            std::fill(std::begin(destination), std::end(destination), uint8_t(0xA5));
            size_t written = 999;
            require(!invalid.SnapshotFrame(destination, sizeof(destination), written) && written == 0,
                    "Malformed null source/header pointers were accepted");
            require(invalid.frameReports == 0 &&
                    std::all_of(std::begin(destination), std::end(destination), [](uint8_t value) { return value == 0xA5; }),
                    "Invalid snapshot wrote data or started the serializer");
        }
        for (bool monsterMode : {false, true})
        {
            c_OutputPixel empty;
            empty.MonsterPermanentRgb = monsterMode;
            empty.pOutputBuffer = source;
            size_t written = 999;
            require(empty.GetWireFrameByteCount() == 0 && !empty.SnapshotFrame(destination, sizeof(destination), written) && written == 0,
                    "Zero-buffer snapshot did not terminate without output");
            require(empty.frameReports == 0, "Zero-buffer snapshot started the serializer");
        }
        c_OutputPixel invalidWidth;
        invalidWidth.pOutputBuffer = source;
        invalidWidth.OutputBufferSize = 3;
        invalidWidth.NumIntensityBytesPerPixel = 0;
        size_t written = 999;
        require(invalidWidth.GetWireFrameByteCount() == 0 &&
                !invalidWidth.SnapshotFrame(destination, sizeof(destination), written) && written == 0 && invalidWidth.frameReports == 0,
                "Zero-width snapshot did not stop before serialization");

        std::cout << "Pixel snapshot serializer passed (" << snapshots << " snapshots, " << checks << " checks).\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
