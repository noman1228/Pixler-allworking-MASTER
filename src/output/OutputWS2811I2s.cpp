#include "output/OutputWS2811I2s.hpp"
#if defined(SUPPORT_PARALLEL_I2S)
#include "output/OutputI2s.hpp"

c_OutputWS2811I2s::c_OutputWS2811I2s(c_OutputMgr::e_OutputChannelIds id, gpio_num_t pin,
                                   uart_port_t port, c_OutputMgr::e_OutputType type) :
    c_OutputWS2811(id, pin, port, type), Lane(uint8_t(id - c_OutputMgr::OutputChannelId_RMT_0))
{
    // Begin/SetConfig must not publish an active output before buffer mapping.
    Paused = true;
}

c_OutputWS2811I2s::~c_OutputWS2811I2s()
{
    if (Registered) { OutputI2s.UnregisterLane(Lane, this); }
}

void c_OutputWS2811I2s::Begin()
{
    c_OutputWS2811::Begin();
    Registered = OutputI2s.RegisterLane(Lane, this);
}

bool c_OutputWS2811I2s::SetConfig(JsonObject& config)
{
    return c_OutputWS2811::SetConfig(config);
}

void c_OutputWS2811I2s::PauseOutput(bool paused)
{
    c_OutputWS2811::PauseOutput(paused);
    if (Registered) { OutputI2s.PauseLane(Lane, this, paused); }
}

void c_OutputWS2811I2s::GetStatus(JsonObject& status)
{
    c_OutputWS2811::GetStatus(status);
    status["backend"] = "I2S parallel";
    JsonObject bus = status["i2s"].to<JsonObject>();
    OutputI2s.GetStatus(bus);
}

bool c_OutputWS2811I2s::DriverIsSendingIntensityData()
{
    return OutputI2s.IsBusy() || !canRefresh();
}
#endif
