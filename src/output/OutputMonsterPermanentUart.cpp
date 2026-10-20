#include "ESPixelStick.h"
#if defined(SUPPORT_OutputType_MonsterPermanent)

#include "output/OutputMonsterPermanentUart.hpp"

static constexpr uint32_t UartBitsPerProtocolBit = 9;
static constexpr uint32_t MonsterUartBaudrate = 800000 * UartBitsPerProtocolBit;

// Inverted 7N1: each UART character encodes one protocol bit in nine ticks.
// Resulting HIGH times are 416.7 ns for zero and 833.3 ns for one.
static const c_OutputUart::ConvertIntensityToUartDataStreamEntry_t MonsterUartTranslation[] =
{
    {0b01111100, c_OutputUart::UartDataBitTranslationId_t::Uart_DATA_BIT_00_ID},
    {0b01100000, c_OutputUart::UartDataBitTranslationId_t::Uart_DATA_BIT_01_ID},
    {0, c_OutputUart::UartDataBitTranslationId_t::Uart_LIST_END}
};

c_OutputMonsterPermanentUart::c_OutputMonsterPermanentUart(c_OutputMgr::e_OutputChannelIds OutputChannelId,
                                                             gpio_num_t outputGpio,
                                                             uart_port_t uart,
                                                             c_OutputMgr::e_OutputType outputType) :
    c_OutputMonsterPermanent(OutputChannelId, outputGpio, uart, outputType)
{
}

c_OutputMonsterPermanentUart::~c_OutputMonsterPermanentUart() = default;

void c_OutputMonsterPermanentUart::Begin()
{
    c_OutputMonsterPermanent::Begin();
    SetIntensityBitTimeInUS(BitTotalNs / float(NanoSecondsInAMicroSecond));

    c_OutputUart::OutputUartConfig_t config;
    config.ChannelId = OutputChannelId;
    config.UartId = UartId;
    config.DataPin = DataPin;
    config.IntensityDataWidth = BitsPerIntensity;
    config.UartDataSize = c_OutputUart::UartDatauint32_t::OUTPUT_UART_7N1;
    config.TranslateIntensityData = c_OutputUart::TranslateIntensityData_t::OneToOne;
    config.pPixelDataSource = this;
    config.Baudrate = MonsterUartBaudrate;
    config.InvertOutputPolarity = true;
    config.CitudsArray = MonsterUartTranslation;
    Uart.Begin(config);
    HasBeenInitialized = true;
}

bool c_OutputMonsterPermanentUart::SetConfig(ArduinoJson::JsonObject &jsonConfig)
{
    bool response = c_OutputMonsterPermanent::SetConfig(jsonConfig);
    response |= Uart.SetConfig(jsonConfig);
    return response;
}

void c_OutputMonsterPermanentUart::GetConfig(ArduinoJson::JsonObject &jsonConfig)
{
    c_OutputMonsterPermanent::GetConfig(jsonConfig);
    Uart.GetConfig(jsonConfig);
}

void c_OutputMonsterPermanentUart::GetStatus(ArduinoJson::JsonObject &jsonStatus)
{
    c_OutputMonsterPermanent::GetStatus(jsonStatus);
    Uart.GetStatus(jsonStatus);
}

uint32_t c_OutputMonsterPermanentUart::Poll()
{
    if (gpio_num_t(-1) == DataPin || !canRefresh()) { return 0; }
    Uart.StartNewFrame();
    return ActualFrameDurationMicroSec;
}

void c_OutputMonsterPermanentUart::PauseOutput(bool State)
{
    c_OutputMonsterPermanent::PauseOutput(State);
    Uart.PauseOutput(State);
}

#endif
