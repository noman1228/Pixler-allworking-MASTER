#pragma once

#include "OutputPixel.hpp"
#if defined(SUPPORT_OutputType_MonsterPermanent)

#ifdef ARDUINO_ARCH_ESP32
#   include <driver/uart.h>
#endif

class c_OutputMonsterPermanent : public c_OutputPixel
{
public:
    c_OutputMonsterPermanent(c_OutputMgr::e_OutputChannelIds OutputChannelId,
                             gpio_num_t outputGpio,
                             uart_port_t uart,
                             c_OutputMgr::e_OutputType outputType);
    virtual ~c_OutputMonsterPermanent();

    virtual void Begin();
    virtual bool SetConfig(ArduinoJson::JsonObject &jsonConfig);
    virtual void GetConfig(ArduinoJson::JsonObject &jsonConfig);
    virtual void GetStatus(ArduinoJson::JsonObject &jsonStatus);
    void GetDriverName(String &sDriverName) { sDriverName = CN_MonsterPermanent; }

protected:
    static constexpr float DataRate = 800000.0f;
    static constexpr float BitTotalNs = 1250.0f;
    static constexpr float ZeroHighNs = 450.0f;
    static constexpr float ZeroLowNs = 800.0f;
    static constexpr float OneHighNs = 850.0f;
    static constexpr float OneLowNs = 400.0f;
    static constexpr uint16_t ResetTimeUs = 80;
    static constexpr uint8_t BitsPerIntensity = 8;
};

#endif
