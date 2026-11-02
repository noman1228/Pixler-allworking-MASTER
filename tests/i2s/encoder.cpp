#include "output/ParallelI2sEncoder.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using ParallelI2sEncoder::Lane;
using ParallelI2sEncoder::Layout;
static unsigned checks = 0;
static unsigned frames = 0;

static void require(bool condition, const char* message)
{
    ++checks;
    if (!condition) { throw std::runtime_error(message); }
}

// Decode each lane's waveform independently from sample masks. A complete
// symbol is recognized by its high pulse length, rather than by the encoder's
// masks/loops. Any activity beyond that lane's actual byte count is rejected.
static void verifyFrame(const Lane (&lanes)[8])
{
    Layout layout;
    require(ParallelI2sEncoder::GetLayout(lanes, layout), "Valid lane layout rejected");
    std::size_t longest = 0;
    std::uint32_t maximumReset = 350;
    for (unsigned lane = 0; lane < 8; ++lane)
    {
        longest = std::max(longest, lanes[lane].size);
        maximumReset = std::max(maximumReset, lanes[lane].resetMicroseconds);
    }
    require(layout.payloadBytes == longest * 8 * 4, "Wrong payload sample count");
    require(layout.totalBytes == layout.payloadBytes + layout.resetBytes, "Wrong total layout size");
    require(layout.totalBytes % 4 == 0 && layout.resetBytes % 4 == 0, "DMA length is not word aligned");
    require(std::uint64_t(layout.resetBytes) * 5 >= std::uint64_t(maximumReset) * 16,
            "Reset duration is shorter than a configured lane gap");
    require(!layout.resetBytes || std::uint64_t(layout.resetBytes - 4) * 5 < std::uint64_t(maximumReset) * 16,
            "Reset padding exceeds the minimum aligned duration");

    std::vector<std::uint8_t> guarded(layout.totalBytes + 18, 0xA5);
    std::size_t written = 999;
    require(ParallelI2sEncoder::Encode(lanes, guarded.data() + 9, layout.totalBytes, written),
            "Valid frame encoding failed");
    require(written == layout.totalBytes, "Written size differs from allocated layout");
    require(std::all_of(guarded.begin(), guarded.begin() + 9, [](std::uint8_t value) { return value == 0xA5; }) &&
            std::all_of(guarded.end() - 9, guarded.end(), [](std::uint8_t value) { return value == 0xA5; }),
            "Encoder changed a destination canary");
    const std::uint8_t* samples = guarded.data() + 9;
    for (unsigned lane = 0; lane < 8; ++lane)
    {
        for (std::size_t byte = 0; byte < longest; ++byte)
        {
            unsigned decoded = 0;
            for (unsigned bit = 0; bit < 8; ++bit)
            {
                const std::size_t start = byte * 32 + bit * 4;
                unsigned highSamples = 0;
                bool lowSeen = false;
                for (unsigned sample = 0; sample < 4; ++sample)
                {
                    const bool high = (samples[start + sample] >> lane) & 1;
                    require(!high || !lowSeen, "High pulse resumed within a symbol");
                    if (high) { ++highSamples; }
                    else { lowSeen = true; }
                }
                if (byte >= lanes[lane].size)
                {
                    require(highSamples == 0, "A shorter lane emitted extra zero data bits");
                }
                else
                {
                    require(highSamples == 1 || highSamples == 3, "Invalid WS2811 pulse width");
                    decoded = (decoded << 1) | (highSamples == 3 ? 1 : 0);
                }
            }
            if (byte < lanes[lane].size)
            {
                require(decoded == lanes[lane].data[byte], "Decoded byte differs from lane payload");
            }
        }
    }
    require(std::all_of(samples + layout.payloadBytes, samples + written,
                        [](std::uint8_t value) { return value == 0; }), "Reset tail is not all low");
    if (layout.totalBytes)
    {
        const std::vector<std::uint8_t> before = guarded;
        written = 999;
        require(!ParallelI2sEncoder::Encode(lanes, guarded.data() + 9, layout.totalBytes - 1, written),
                "A short destination was accepted");
        require(written == 0 && guarded == before, "Capacity rejection modified the destination");
        require(!ParallelI2sEncoder::Encode(lanes, nullptr, layout.totalBytes, written) && written == 0,
                "A null nonempty destination was accepted");
    }
    ++frames;
}

static std::uint32_t randomState = 0x6A09E667U;
static std::uint32_t nextRandom()
{
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return randomState;
}

int main()
{
    try
    {
        Lane empty[8];
        verifyFrame(empty);
        std::size_t written = 999;
        require(!ParallelI2sEncoder::Encode(empty, nullptr, 0, written) && written == 0,
                "A reset-only frame accepted a missing destination");

        // Golden sample masks establish lane order and MSB-first bit order.
        const std::uint8_t zero = 0, highBit = 0x80, ones = 0xFF;
        Lane golden[8];
        golden[0] = Lane(&zero, 1, 1);
        golden[3] = Lane(&highBit, 1, 2);
        golden[7] = Lane(&ones, 1, 3);
        std::uint8_t expected[32 + 1120] = {};
        expected[0] = 0x89;
        expected[1] = expected[2] = 0x88;
        for (unsigned symbol = 1; symbol < 8; ++symbol)
        {
            expected[symbol * 4] = 0x89;
            expected[symbol * 4 + 1] = expected[symbol * 4 + 2] = 0x80;
        }
        std::uint8_t encoded[sizeof(expected)];
        require(ParallelI2sEncoder::Encode(golden, encoded, sizeof(encoded), written), "Golden frame rejected");
        require(written == sizeof(expected) && std::equal(std::begin(expected), std::end(expected), std::begin(encoded)),
                "Golden chronological sample bytes differ");
        verifyFrame(golden);

        for (unsigned gap : {0U, 1U, 349U, 350U, 351U, 354U, 5000U})
        {
            Lane resetBoundary[8];
            resetBoundary[2] = Lane(&zero, 1, gap);
            verifyFrame(resetBoundary);
        }

        // Exercise every byte value on each physical lane, including lanes
        // whose neighbors terminate earlier or remain completely inactive.
        std::array<std::vector<std::uint8_t>, 8> storage;
        for (unsigned lane = 0; lane < 8; ++lane)
        {
            storage[lane].resize(256);
            for (unsigned value = 0; value < 256; ++value) { storage[lane][value] = std::uint8_t(value); }
            Lane isolated[8];
            isolated[lane] = Lane(storage[lane].data(), storage[lane].size(), 300 + lane);
            verifyFrame(isolated);
        }
        Lane all[8];
        for (unsigned lane = 0; lane < 8; ++lane) { all[lane] = Lane(storage[lane].data(), 256 - lane * 31, lane * 17); }
        verifyFrame(all);

        for (unsigned iteration = 0; iteration < 400; ++iteration)
        {
            Lane lanes[8];
            for (unsigned lane = 0; lane < 8; ++lane)
            {
                const unsigned length = nextRandom() % 258;
                storage[lane].resize(length);
                for (unsigned byte = 0; byte < length; ++byte) { storage[lane][byte] = std::uint8_t(nextRandom()); }
                lanes[lane] = Lane(length ? storage[lane].data() : nullptr, length, nextRandom() % 2001);
            }
            const auto original = storage;
            verifyFrame(lanes);
            require(storage == original, "Encoder modified an input lane");
        }

        Layout layout;
        Lane invalid[8];
        invalid[4] = Lane(nullptr, 1, 300);
        require(!ParallelI2sEncoder::GetLayout(invalid, layout) && layout.totalBytes == 0,
                "A null nonempty lane was accepted");
        std::uint8_t destination[32];
        std::fill(std::begin(destination), std::end(destination), std::uint8_t(0xA5));
        require(!ParallelI2sEncoder::Encode(invalid, destination, sizeof(destination), written) && written == 0 &&
                std::all_of(std::begin(destination), std::end(destination), [](std::uint8_t value) { return value == 0xA5; }),
                "Invalid-source rejection changed the output");

        const std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
        invalid[4] = Lane(&zero, maximum / 32 + 1);
        require(!ParallelI2sEncoder::GetLayout(invalid, layout), "Payload multiplication overflow was accepted");
        invalid[4] = Lane(&zero, maximum / 32, 10);
        require(!ParallelI2sEncoder::GetLayout(invalid, layout), "Payload plus reset overflow was accepted");
        Lane maximumGap[8];
        maximumGap[0].resetMicroseconds = (std::numeric_limits<std::uint32_t>::max)();
        const std::uint64_t neededReset =
            ((std::uint64_t(maximumGap[0].resetMicroseconds) * 32 + 9) / 10 + 3) / 4 * 4;
        const bool fits = neededReset <= maximum;
        require(ParallelI2sEncoder::GetLayout(maximumGap, layout) == fits, "Maximum reset overflow handled incorrectly");
        if (fits) { require(layout.resetBytes == neededReset, "Maximum reset calculation wrapped"); }

        std::cout << "Parallel I2S encoder passed (" << frames << " frames, " << checks << " checks).\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
