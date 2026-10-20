#pragma once

#include "ESPixelStick.h"
#if defined(SUPPORT_OutputType_MonsterPermanent) && defined(ARDUINO_ARCH_ESP32)

#include "OutputMonsterPermanent.hpp"
#include "OutputRmt.hpp"

class c_OutputMonsterPermanentRmt : public c_OutputMonsterPermanent
{
public:
    c_OutputMonsterPermanentRmt(c_OutputMgr::e_OutputChannelIds OutputChannelId,
                                gpio_num_t outputGpio,
                                uart_port_t uart,
                                c_OutputMgr::e_OutputType outputType);
    virtual ~c_OutputMonsterPermanentRmt();

    void Begin();
    bool SetConfig(ArduinoJson::JsonObject &jsonConfig);
    uint32_t Poll();
    bool RmtPoll();
    void GetStatus(ArduinoJson::JsonObject &jsonStatus);
    void SetOutputBufferSize(uint32_t NumChannelsAvailable);
    bool DriverIsSendingIntensityData() { return Rmt.DriverIsSendingIntensityData() || !canRefresh(); }
    void PauseOutput(bool State);

private:
    c_OutputRmt Rmt;
};

#endif
