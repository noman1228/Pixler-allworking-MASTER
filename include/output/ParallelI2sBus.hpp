#pragma once

#if defined(ARDUINO_ARCH_ESP32) && defined(SUPPORT_PARALLEL_I2S)
#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32)

#include <stddef.h>
#include <stdint.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_intr_alloc.h>
#include <esp32/rom/lldesc.h>

// Classic ESP32 I2S1, eight parallel bits at 3.2 MHz. All task-context methods
// belong to the same owner task; only the completion ISR runs asynchronously.
// I2S1 is reserved exclusively while Begin/End are active.
class c_ParallelI2sBus
{
public:
    static constexpr size_t LaneCount = 8;
    static constexpr uint32_t SamplesPerSecond = 3200000;

    c_ParallelI2sBus() = default;
    ~c_ParallelI2sBus();
    c_ParallelI2sBus(const c_ParallelI2sBus&) = delete;
    c_ParallelI2sBus& operator=(const c_ParallelI2sBus&) = delete;

    // -1 leaves a lane disconnected. Other lanes retain their existing owners.
    bool Begin(const gpio_num_t pins[LaneCount]);
    void End();

    // The caller owns this DMA-capable, 4-byte-aligned buffer and keeps it
    // immutable until IsBusy() is false. It must include the full zero reset
    // tail, with length a multiple of four. No samples are copied or changed.
    bool Send(const uint8_t* dmaSamples, size_t bytes);
    bool IsBusy() const;
    bool Wait(uint32_t timeoutMs);
    // Abort a transfer and hold registered lane pins low. Does not touch the
    // pins of disconnected lanes and does not free the caller's frame buffer.
    void Stop();

    bool HadDmaError() const { return DmaError; }
    esp_err_t LastError() const { return Error; }

    // I2S1's 8-bit FIFO swaps adjacent 16-bit halves of each DMA word. Store
    // logical sample i at this byte offset before handing the buffer to Send.
    static constexpr size_t DmaByteIndex(size_t logicalSample) { return logicalSample ^ size_t(2); }

private:
    static void IRAM_ATTR InterruptHandler(void* argument);
    static void IRAM_ATTR HaltHardware();
    void HoldPinsLow();
    void AttachPins();

    gpio_num_t Pins[LaneCount] = {gpio_num_t(-1), gpio_num_t(-1), gpio_num_t(-1), gpio_num_t(-1),
                                 gpio_num_t(-1), gpio_num_t(-1), gpio_num_t(-1), gpio_num_t(-1)};
    lldesc_t* Descriptors = nullptr;
    size_t DescriptorCapacity = 0;
    intr_handle_t Interrupt = nullptr;
    bool Initialized = false;
    bool PinsAttached = false;
    volatile bool Busy = false;
    volatile bool DmaEof = false;
    volatile bool DmaError = false;
    volatile esp_err_t Error = ESP_OK;
};

#endif // CONFIG_IDF_TARGET_ESP32
#endif // ARDUINO_ARCH_ESP32 && SUPPORT_PARALLEL_I2S
