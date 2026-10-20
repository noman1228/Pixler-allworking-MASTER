#include "ESPixelStick.h"
#if defined(SUPPORT_OutputType_MonsterPermanent) && defined(ARDUINO_ARCH_ESP32)

#include "output/OutputMonsterPermanentRmt.hpp"

#define MONSTER_RMT_TICKS_0_HIGH uint16_t(450.0 / RMT_TickLengthNS)
#define MONSTER_RMT_TICKS_0_LOW  uint16_t(800.0 / RMT_TickLengthNS)
#define MONSTER_RMT_TICKS_1_HIGH uint16_t(850.0 / RMT_TickLengthNS)
#define MONSTER_RMT_TICKS_1_LOW  uint16_t(400.0 / RMT_TickLengthNS)

static const c_OutputRmt::ConvertIntensityToRmtDataStreamEntry_t MonsterRmtTranslation[] =
{
    {{MONSTER_RMT_TICKS_0_HIGH, 1, MONSTER_RMT_TICKS_0_LOW, 0}, c_OutputRmt::RmtDataBitIdType_t::RMT_DATA_BIT_ZERO_ID},
    {{MONSTER_RMT_TICKS_1_HIGH, 1, MONSTER_RMT_TICKS_1_LOW, 0}, c_OutputRmt::RmtDataBitIdType_t::RMT_DATA_BIT_ONE_ID},
    {{1, 0, 1, 0}, c_OutputRmt::RmtDataBitIdType_t::RMT_INTERFRAME_GAP_ID},
    {{2, 1, 2, 1}, c_OutputRmt::RmtDataBitIdType_t::RMT_STARTBIT_ID},
    {{0, 0, 0, 0}, c_OutputRmt::RmtDataBitIdType_t::RMT_STOPBIT_ID},
    {{0, 0, 0, 0}, c_OutputRmt::RmtDataBitIdType_t::RMT_LIST_END}
};

c_OutputMonsterPermanentRmt::c_OutputMonsterPermanentRmt(c_OutputMgr::e_OutputChannelIds OutputChannelId,
                                                           gpio_num_t outputGpio,
                                                           uart_port_t uart,
                                                           c_OutputMgr::e_OutputType outputType) :
    c_OutputMonsterPermanent(OutputChannelId, outputGpio, uart, outputType)
{
}

c_OutputMonsterPermanentRmt::~c_OutputMonsterPermanentRmt() = default;

void c_OutputMonsterPermanentRmt::Begin()
{
    c_OutputMonsterPermanent::Begin();
    HasBeenInitialized = true;
}

bool c_OutputMonsterPermanentRmt::SetConfig(ArduinoJson::JsonObject &jsonConfig)
{
    bool response = c_OutputMonsterPermanent::SetConfig(jsonConfig);

    uint32_t ifgTicks = (uint32_t(InterFrameGapInMicroSec) * NanoSecondsInAMicroSecond) / RMT_TickLengthNS;
    rmt_item32_t idle;
    idle.duration0 = ifgTicks / 2;
    idle.level0 = 0;
    idle.duration1 = ifgTicks - idle.duration0;
    idle.level1 = 0;

    c_OutputRmt::OutputRmtConfig_t config;
    config.RmtChannelId = rmt_channel_t(OutputChannelId);
    config.DataPin = gpio_num_t(DataPin);
    config.idle_level = rmt_idle_level_t::RMT_IDLE_LEVEL_LOW;
    config.pPixelDataSource = this;
    config.NumFrameStartBits = 0;
    config.CitrdsArray = MonsterRmtTranslation;
    config.NumIdleBits = 1;

    Rmt.Begin(config, this);
    Rmt.ValidateBitXlatTable(MonsterRmtTranslation);
    Rmt.SetIntensity2Rmt(idle, c_OutputRmt::RmtDataBitIdType_t::RMT_INTERFRAME_GAP_ID);
    return response;
}

void c_OutputMonsterPermanentRmt::SetOutputBufferSize(uint32_t NumChannelsAvailable)
{
    c_OutputMonsterPermanent::SetOutputBufferSize(NumChannelsAvailable);
}

void c_OutputMonsterPermanentRmt::GetStatus(ArduinoJson::JsonObject &jsonStatus)
{
    c_OutputMonsterPermanent::GetStatus(jsonStatus);
    Rmt.GetStatus(jsonStatus);
}

uint32_t c_OutputMonsterPermanentRmt::Poll()
{
    return ActualFrameDurationMicroSec;
}

bool c_OutputMonsterPermanentRmt::RmtPoll()
{
    if (gpio_num_t(-1) == DataPin) { return false; }
    ReportNewFrame();
    return Rmt.StartNewFrame();
}

void c_OutputMonsterPermanentRmt::PauseOutput(bool State)
{
    c_OutputMonsterPermanent::PauseOutput(State);
    Rmt.PauseOutput(State);
}

#endif
