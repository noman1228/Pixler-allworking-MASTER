#include "../unzip/shims/ESPixelStick.h"
#include "output/ParallelI2sEncoder.hpp"
#include <set>
#include <utility>

static unsigned checks = 0;
static void require(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
using gpio_num_t = int;
static uint32_t micros() { return testClock * 1000u; }
static constexpr unsigned MALLOC_CAP_DMA = 1, MALLOC_CAP_INTERNAL = 2;
static bool failSnapshotAllocation = false, failDmaAllocation = false;
static std::set<void*> allocations;

struct c_ParallelI2sBus {
    bool busy = false, dmaError = false, failBegin = false, failSend = false;
    int pins[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    const uint8_t* dma = nullptr;
    size_t bytes = 0;
    unsigned sends = 0, ends = 0, stops = 0;
    std::vector<uint8_t> accepted;
    bool Begin(const int* requested) {
        if (failBegin) return false;
        std::copy_n(requested, 8, pins);
        dmaError = false;
        return true;
    }
    void End() { ++ends; Stop(); }
    void Stop() { ++stops; busy = false; }
    bool HadDmaError() const { return dmaError; }
    bool IsBusy() const { return busy; }
    bool Send(const uint8_t* data, size_t count) {
        if (failSend) return false;
        require(!busy, "A frame replaced active DMA");
        dma = data;
        bytes = count;
        accepted.assign(data, data + count);
        busy = true;
        ++sends;
        return true;
    }
};
static c_ParallelI2sBus* observedBus = nullptr;
static void* testMalloc(size_t count) {
    if (failSnapshotAllocation) return nullptr;
    void* result = std::malloc(count);
    if (result) allocations.insert(result);
    return result;
}
static void testFree(void* pointer) {
    if (!pointer) return;
    require(!observedBus || !observedBus->busy, "A live DMA buffer was freed before stopping the bus");
    require(allocations.erase(pointer) == 1, "An unowned frame buffer was freed");
    std::free(pointer);
}
static void* heap_caps_malloc(size_t count, unsigned flags) {
    require(flags == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), "Waveform was not allocated in internal DMA memory");
    if (failDmaAllocation) return nullptr;
    return testMalloc(count);
}
static void heap_caps_free(void* pointer) { testFree(pointer); }

struct c_OutputWS2811I2s {
    bool paused = true, ready = true, failSnapshot = false;
    int pin = 0;
    uint32_t reset = 350;
    unsigned snapshots = 0;
    std::vector<uint8_t> wire;
    bool IsPaused() const { return paused; }
    size_t GetBufferUsedSize() const { return wire.size(); }
    bool IsFrameReady() const { return ready; }
    uint64_t GetWireFrameByteCount() const { return wire.size(); }
    int GetOutputGpio() const { return pin; }
    uint32_t GetResetTimeUs() const { return reset; }
    bool SnapshotFrame(uint8_t* data, size_t capacity, size_t& written) {
        ++snapshots;
        written = 0;
        if (failSnapshot || capacity < wire.size()) return false;
        std::copy(wire.begin(), wire.end(), data);
        written = wire.size();
        return true;
    }
};

#define private public
#include "coordinator_class.inc"
#undef private
#define malloc testMalloc
#define free testFree
#include "coordinator_methods.inc"
#undef malloc
#undef free

static void decodeLane(const c_ParallelI2sBus& bus, unsigned lane, const std::vector<uint8_t>& expected) {
    const auto logical = [&](size_t i) { return bus.dma[i ^ 2]; };
    for (size_t byte = 0; byte < expected.size(); ++byte) {
        uint8_t decoded = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const size_t index = byte * 32 + bit * 4;
            const uint8_t mask = uint8_t(1u << lane);
            require(logical(index) & mask, "Data bit did not start high");
            require(!(logical(index + 3) & mask), "Data bit did not end low");
            const bool one = logical(index + 1) & mask;
            require(bool(logical(index + 2) & mask) == one, "I2S half-word ordering broke pulse shape");
            decoded = uint8_t((decoded << 1) | unsigned(one));
        }
        require(decoded == expected[byte], "DMA sample order changed serialized pixel data");
    }
    for (size_t sample = expected.size() * 32; sample < bus.bytes; ++sample)
        require(!(logical(sample) & uint8_t(1u << lane)), "Finished lane or reset tail was driven high");
}

int main() {
    try {
        c_OutputI2s coordinator;
        observedBus = &coordinator.Bus;
        auto& bus = coordinator.Bus;
        c_OutputWS2811I2s first, last, other;
        first.pin = 0; first.wire = {0x80, 0x01, 0x55};
        last.pin = 13; last.wire = {0xaa}; last.reset = 500;
        other.pin = -1; other.wire = {0xff}; other.paused = false; other.ready = false;
        require(!coordinator.RegisterLane(8, &other) && coordinator.Failed,
                "Invalid lane index was accepted");
        coordinator.PrepareForConfig();
        require(coordinator.RegisterLane(0, &first) && coordinator.RegisterLane(7, &last), "Valid lane registration failed");
        require(!coordinator.RegisterLane(0, &other) && coordinator.Sources[0] == &first,
                "Duplicate registration replaced an existing owner");
        coordinator.PrepareForConfig();
        require(coordinator.RegisterLane(3, &other), "Intentionally disconnected lane registration failed");
        coordinator.Poll();
        require(bus.sends == 0 && !coordinator.Snapshot && !coordinator.DmaFrame, "Unmapped paused output started DMA");
        first.paused = last.paused = false;
        last.ready = false;
        coordinator.Poll();
        require(first.snapshots == 0 && bus.sends == 0, "A not-ready lane advanced other frame serializers");
        last.ready = true;
        coordinator.Poll();
        require(bus.sends == 1 && bus.busy && bus.pins[0] == 0 && bus.pins[7] == 13, "Ready lanes did not share one DMA frame");
        require(other.snapshots == 0 && coordinator.SnapshotCapacity == 4, "GPIO -1 lane blocked or contributed data to the frame");
        for (unsigned lane = 1; lane < 7; ++lane) require(bus.pins[lane] == -1, "Inactive lane stole another protocol's GPIO");
        coordinator.UnregisterLane(0, &other);
        coordinator.PauseLane(0, &other, true);
        require(bus.busy && coordinator.Sources[0] == &first, "A non-owner stopped or removed a lane");
        decodeLane(bus, 0, first.wire);
        decodeLane(bus, 7, last.wire);
        const auto snapshot = std::vector<uint8_t>(coordinator.Snapshot, coordinator.Snapshot + 4);
        const auto waveform = bus.accepted;
        first.wire.assign(30, 0xff);
        last.wire.assign(2, 0x00);
        for (unsigned repeat = 0; repeat < 5; ++repeat) coordinator.Poll();
        require(bus.sends == 1 && first.snapshots == 1 && last.snapshots == 1, "Busy DMA advanced a serializer or started another frame");
        require(std::equal(waveform.begin(), waveform.end(), bus.dma), "Active DMA waveform was rewritten");
        require(std::equal(snapshot.begin(), snapshot.end(), coordinator.Snapshot), "Active frame snapshot was rewritten");
        bus.busy = false;
        coordinator.Poll();
        require(bus.sends == 2 && first.snapshots == 2, "Idle DMA did not accept the latest source frame");
        decodeLane(bus, 0, first.wire);
        coordinator.UnregisterLane(0, &first);
        require(!bus.busy && !coordinator.Snapshot && !coordinator.DmaFrame && !coordinator.Sources[0], "Unregister released ownership before stopping DMA");
        coordinator.Poll();
        require(bus.pins[0] == -1 && bus.pins[7] == 13, "Protocol change retained the old parallel GPIO");
        last.paused = true;
        coordinator.PauseLane(7, &last, true);
        coordinator.Poll();
        require(!bus.busy && !coordinator.HasPins && !coordinator.DmaFrame, "Pausing all lanes retained an active waveform");

        for (unsigned fault = 0; fault < 7; ++fault) {
            coordinator.PrepareForConfig();
            first.wire = {0x42}; first.paused = false;
            last.paused = true;
            require(coordinator.RegisterLane(0, &first), "Retry could not register an output lane");
            failSnapshotAllocation = fault == 0;
            failDmaAllocation = fault == 1;
            bus.failBegin = fault == 2;
            first.failSnapshot = fault == 3;
            bus.failSend = fault == 4;
            const unsigned sends = bus.sends;
            const uint32_t frames = coordinator.FramesSent;
            if (fault == 6) testClock = UINT32_MAX / 1000u - 20;
            coordinator.Poll();
            if (fault == 5) { bus.dmaError = true; bus.busy = false; coordinator.Poll(); }
            if (fault == 6) { testClock += 200; coordinator.Poll(); }
            require(coordinator.Failed && !bus.busy && coordinator.Error, "Transport fault did not stop safely and expose an error");
            require(fault >= 5 || bus.sends == sends, "Refused configuration transmitted a frame");
            require(coordinator.FramesSent == frames + unsigned(fault >= 5), "Transport failure incorrectly advanced frame counters");
            JsonDocument statusDocument;
            JsonObject status = statusDocument.to<JsonObject>();
            coordinator.GetStatus(status);
            require(!status["error"].isNull() && !status["busy"].as<bool>(), "Transport failure was not exposed in status");
            const auto calls = first.snapshots;
            coordinator.Poll();
            require(first.snapshots == calls, "Failed output retried without explicit configuration");
            failSnapshotAllocation = failDmaAllocation = bus.failBegin = bus.failSend = first.failSnapshot = false;
            coordinator.PrepareForConfig();
            coordinator.Poll();
            require(!coordinator.Failed && bus.busy, "Saving output settings could not recover from a prior transport fault");
        }
        coordinator.PrepareForConfig();
        require(allocations.empty(), "Frame storage leaked after configuration cleanup");
        std::cout << "I2S coordinator ownership passed: " << checks << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
