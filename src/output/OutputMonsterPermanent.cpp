#include "ESPixelStick.h"
#include "output/OutputMonsterPermanent.hpp"
#if defined(SUPPORT_OutputType_MonsterPermanent)

c_OutputMonsterPermanent::c_OutputMonsterPermanent(c_OutputMgr::e_OutputChannelIds OutputChannelId,
                                                     gpio_num_t outputGpio,
                                                     uart_port_t uart,
                                                     c_OutputMgr::e_OutputType outputType) :
    c_OutputPixel(OutputChannelId, outputGpio, uart, outputType)
{
    InterFrameGapInMicroSec = ResetTimeUs;
    EnableMonsterPermanentRgb();
}

c_OutputMonsterPermanent::~c_OutputMonsterPermanent() = default;

void c_OutputMonsterPermanent::Begin()
{
    c_OutputPixel::Begin();
    HasBeenInitialized = true;
}

void c_OutputMonsterPermanent::GetConfig(ArduinoJson::JsonObject &jsonConfig)
{
    c_OutputPixel::GetConfig(jsonConfig);
}

void c_OutputMonsterPermanent::GetStatus(ArduinoJson::JsonObject &jsonStatus)
{
    c_OutputPixel::GetStatus(jsonStatus);
}

bool c_OutputMonsterPermanent::SetConfig(ArduinoJson::JsonObject &jsonConfig)
{
    // The software-facing representation remains one RGB triplet per module.
    jsonConfig[CN_color_order] = "rgb";
    uint32_t gap = ResetTimeUs;
    setFromJSON(gap, jsonConfig, CN_interframetime);
    jsonConfig[CN_interframetime] = max(uint32_t(ResetTimeUs), min(gap, uint32_t(10000)));

    bool response = c_OutputPixel::SetConfig(jsonConfig);
    SetFrameDurration(BitTotalNs / float(NanoSecondsInAMicroSecond));
    return response;
}

#endif
