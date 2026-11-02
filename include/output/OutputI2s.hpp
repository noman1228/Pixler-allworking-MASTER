#pragma once

#include "ESPixelStick.h"
#if defined(SUPPORT_PARALLEL_I2S)
#include "ParallelI2sBus.hpp"

class c_OutputWS2811I2s;

// OutputMgr excludes traffic using its data lock or its configuration state.
// The hardware DMA reads a separate frame, never the live pixel buffers.
class c_OutputI2s
{
public:
    bool RegisterLane(uint8_t lane, c_OutputWS2811I2s* source);
    void UnregisterLane(uint8_t lane, c_OutputWS2811I2s* source);
    void PauseLane(uint8_t lane, c_OutputWS2811I2s* source, bool paused);
    void PrepareForConfig();
    void Poll();
    bool IsBusy() const { return Bus.IsBusy(); }
    void GetStatus(JsonObject& status);
    void GetDriverName(String& name) { name = "I2S"; }

private:
    c_ParallelI2sBus Bus;
    c_OutputWS2811I2s* Sources[8] = {};
    gpio_num_t Pins[8] = {};
    bool HasPins = false;
    bool Failed = false;
    const char* Error = nullptr;
    uint8_t ActiveMask = 0;
    uint8_t* Snapshot = nullptr;
    size_t SnapshotCapacity = 0;
    uint8_t* DmaFrame = nullptr;
    size_t DmaCapacity = 0;
    size_t FrameBytes = 0;
    uint32_t FramesSent = 0;
    uint32_t SentAtUs = 0;
    void SetError(const char* message);
};

extern c_OutputI2s OutputI2s;
#endif
