#pragma once

#include "ESPixelStick.h"
#if defined(SUPPORT_PARALLEL_I2S)
#if !defined(ARDUINO_ARCH_ESP32) || !defined(CONFIG_IDF_TARGET_ESP32) || !defined(BOARD_ESP32_QUINLED_DIG_OCTA)
#error Parallel I2S output requires the classic ESP32 Dig Octa target
#endif

#include "OutputWS2811.hpp"

class c_OutputWS2811I2s : public c_OutputWS2811
{
public:
    c_OutputWS2811I2s(c_OutputMgr::e_OutputChannelIds id, gpio_num_t pin,
                     uart_port_t port, c_OutputMgr::e_OutputType type);
    ~c_OutputWS2811I2s() override;
    void Begin() override;
    bool SetConfig(JsonObject& config) override;
    uint32_t Poll() override { return ActualFrameDurationMicroSec; }
    bool RmtPoll() override { return false; }
    void PauseOutput(bool paused) override;
    void GetStatus(JsonObject& status) override;
    bool DriverIsSendingIntensityData() override;
    bool IsFrameReady() { return HasBeenInitialized && !Paused && pOutputBuffer && OutputBufferSize && canRefresh(); }
    uint32_t GetResetTimeUs() const { return max(uint32_t(350), uint32_t(InterFrameGapInMicroSec)); }

private:
    uint8_t Lane;
    bool Registered = false;
};
#endif
