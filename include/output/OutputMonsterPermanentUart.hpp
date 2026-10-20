#pragma once

#include "ESPixelStick.h"
#if defined(SUPPORT_OutputType_MonsterPermanent)

#include "OutputMonsterPermanent.hpp"
#include "OutputUart.hpp"

class c_OutputMonsterPermanentUart : public c_OutputMonsterPermanent
{
public:
    c_OutputMonsterPermanentUart(c_OutputMgr::e_OutputChannelIds OutputChannelId,
                                 gpio_num_t outputGpio,
                                 uart_port_t uart,
                                 c_OutputMgr::e_OutputType outputType);
    virtual ~c_OutputMonsterPermanentUart();

    void Begin();
    uint32_t Poll();
#if defined(ARDUINO_ARCH_ESP32)
    bool RmtPoll() { return false; }
#endif
    void PauseOutput(bool State);
    bool SetConfig(ArduinoJson::JsonObject &jsonConfig);
    void GetConfig(ArduinoJson::JsonObject &jsonConfig);
    void GetStatus(ArduinoJson::JsonObject &jsonStatus);

private:
    c_OutputUart Uart;
};

#endif
