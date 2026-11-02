#include "output/OutputI2s.hpp"
#if defined(SUPPORT_PARALLEL_I2S)
#include "output/OutputWS2811I2s.hpp"
#include "output/ParallelI2sEncoder.hpp"
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <utility>

c_OutputI2s OutputI2s;

void c_OutputI2s::SetError(const char* message)
{
    Bus.Stop();
    Failed = true;
    if (Error != message) { logcon(String(F("Parallel output stopped: ")) + message); }
    Error = message;
}

void c_OutputI2s::PrepareForConfig()
{
    // Stop before releasing buffers or letting a different protocol own a pin.
    Bus.End();
    free(Snapshot);
    heap_caps_free(DmaFrame);
    Snapshot = DmaFrame = nullptr;
    SnapshotCapacity = DmaCapacity = FrameBytes = 0;
    ActiveMask = 0;
    HasPins = Failed = false;
    Error = nullptr;
}

bool c_OutputI2s::RegisterLane(uint8_t lane, c_OutputWS2811I2s* source)
{
    if (lane >= 8 || !source || (Sources[lane] && Sources[lane] != source))
    {
        SetError("Invalid or duplicate I2S lane");
        return false;
    }
    PrepareForConfig();
    Sources[lane] = source;
    return true;
}

void c_OutputI2s::UnregisterLane(uint8_t lane, c_OutputWS2811I2s* source)
{
    if (lane >= 8 || Sources[lane] != source) { return; }
    PrepareForConfig();
    Sources[lane] = nullptr;
}

void c_OutputI2s::PauseLane(uint8_t lane, c_OutputWS2811I2s* source, bool paused)
{
    if (lane < 8 && Sources[lane] == source && paused) { Bus.Stop(); }
}

void c_OutputI2s::Poll()
{
    if (Failed) { return; }
    if (HasPins && Bus.HadDmaError()) { SetError("I2S DMA error; save output settings to retry"); return; }
    if (Bus.IsBusy())
    {
        const uint64_t expectedUs = (uint64_t(FrameBytes) * 5 + 15) / 16;
        if (uint32_t(micros() - SentAtUs) > expectedUs + 100000)
        {
            SetError("I2S DMA timeout; save output settings to retry");
        }
        return;
    }

    size_t sizes[8] = {};
    gpio_num_t pins[8];
    uint8_t mask = 0;
    size_t total = 0;
    for (uint8_t lane = 0; lane < 8; ++lane)
    {
        pins[lane] = gpio_num_t(-1);
        auto* source = Sources[lane];
        if (!source || source->IsPaused() || !source->GetBufferUsedSize() ||
            source->GetOutputGpio() == gpio_num_t(-1)) { continue; }
        if (!source->IsFrameReady()) { return; }
        const uint64_t bytes = source->GetWireFrameByteCount();
        if (!bytes || bytes > SIZE_MAX || size_t(bytes) > SIZE_MAX - total)
        {
            SetError("I2S frame size exceeds addressable memory"); return;
        }
        sizes[lane] = size_t(bytes);
        total += sizes[lane];
        pins[lane] = source->GetOutputGpio();
        mask |= uint8_t(1U << lane);
    }
    if (!mask) { if (HasPins) { PrepareForConfig(); } return; }

    if (total > SnapshotCapacity)
    {
        free(Snapshot);
        Snapshot = static_cast<uint8_t*>(malloc(total));
        SnapshotCapacity = Snapshot ? total : 0;
        if (!Snapshot) { SetError("Insufficient RAM for I2S frame snapshot"); return; }
    }
    ParallelI2sEncoder::Lane lanes[8] = {};
    size_t offset = 0;
    for (uint8_t lane = 0; lane < 8; ++lane)
    {
        if (!sizes[lane]) { continue; }
        lanes[lane].data = Snapshot + offset;
        lanes[lane].size = sizes[lane];
        lanes[lane].resetMicroseconds = Sources[lane]->GetResetTimeUs();
        offset += sizes[lane];
    }
    ParallelI2sEncoder::Layout layout;
    if (!ParallelI2sEncoder::GetLayout(lanes, layout)) { SetError("Invalid I2S frame layout"); return; }
    if (layout.totalBytes > DmaCapacity)
    {
        heap_caps_free(DmaFrame);
        DmaFrame = static_cast<uint8_t*>(heap_caps_malloc(layout.totalBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        DmaCapacity = DmaFrame ? layout.totalBytes : 0;
        if (!DmaFrame) { SetError("Insufficient internal DMA RAM; reduce longest I2S output"); return; }
    }

    bool changed = !HasPins;
    for (uint8_t lane = 0; lane < 8; ++lane) { changed |= Pins[lane] != pins[lane]; }
    if (changed)
    {
        if (!Bus.Begin(pins)) { SetError("Could not initialize parallel I2S pins or peripheral"); return; }
        for (uint8_t lane = 0; lane < 8; ++lane) { Pins[lane] = pins[lane]; }
        HasPins = true;
    }

    for (uint8_t lane = 0; lane < 8; ++lane)
    {
        if (!sizes[lane]) { continue; }
        size_t written;
        if (!Sources[lane]->SnapshotFrame(const_cast<uint8_t*>(lanes[lane].data), sizes[lane], written) || written != sizes[lane])
        {
            SetError("I2S pixel snapshot did not match its frame size"); return;
        }
    }
    size_t written;
    if (!ParallelI2sEncoder::Encode(lanes, DmaFrame, DmaCapacity, written)) { SetError("Could not encode I2S frame"); return; }
    // I2S1 reads each 32-bit word as byte indices 2,3,0,1.
    for (size_t index = 0; index < written; index += 4)
    {
        std::swap(DmaFrame[index], DmaFrame[index + 2]);
        std::swap(DmaFrame[index + 1], DmaFrame[index + 3]);
    }
    if (!Bus.Send(DmaFrame, written)) { SetError("Could not start I2S DMA frame"); return; }
    FrameBytes = written;
    ActiveMask = mask;
    SentAtUs = micros();
    ++FramesSent;
}

void c_OutputI2s::GetStatus(JsonObject& status)
{
    status["lanes"] = ActiveMask;
    status["frames"] = FramesSent;
    status["frameBytes"] = FrameBytes;
    status["dmaAllocated"] = DmaCapacity;
    status["busy"] = Bus.IsBusy();
    if (Error) { status["error"] = Error; }
}
#endif
