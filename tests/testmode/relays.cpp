#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#define ARDUINO_ARCH_ESP32
#define DEBUG_START
#define DEBUG_END
#define DEBUG_V(...)
using gpio_num_t = int;
static std::map<int, uint8_t> digitalLevels, pwmLevels;
static uint32_t writes = 0;
static void digitalWrite(uint8_t pin, uint8_t value) { digitalLevels[pin] = value; ++writes; }
static void ledcWrite(uint8_t channel, uint8_t value) { pwmLevels[channel] = value; ++writes; }
static long map(long value, long inMin, long inMax, long outMin, long outMax)
{
    return (value - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}
struct c_OutputRelay
{
    struct RelayChannel_t
    {
        bool Enabled = true;
        bool httpEnabled = true;
        bool Pwm = false;
        uint8_t OnOffTriggerLevel = 127;
        gpio_num_t GpioId = -1;
        uint8_t OnValue = 1;
        uint8_t OffValue = 0;
        uint8_t previousValue = 0;
        uint8_t ChannelIndex = 0;
    };
    RelayChannel_t OutputList[8];
    uint8_t TestModeSavedValues[8]{};
    bool TestModePaused = false;
    void OutputValue(RelayChannel_t& currentRelay, uint8_t NewValue);
    void SetTestModePaused(bool paused, bool restore = true);
};
#include "relay_methods.inc"

int main()
{
    c_OutputRelay relays;
    for (uint8_t index = 0; index < 8; ++index)
    {
        auto& relay = relays.OutputList[index];
        relay.GpioId = 10 + index;
        relay.ChannelIndex = index;
        relay.previousValue = 1;
        digitalLevels[uint8_t(relay.GpioId)] = relay.previousValue;
    }
    relays.OutputList[1].OffValue = 1;
    relays.OutputList[1].OnValue = 0;
    relays.OutputList[1].previousValue = 0;
    relays.OutputList[2].Pwm = true;
    relays.OutputList[2].OnValue = 255;
    relays.OutputList[2].previousValue = 123;
    relays.OutputList[3].Pwm = true;
    relays.OutputList[3].OffValue = 255;
    relays.OutputList[3].OnValue = 0;
    relays.OutputList[3].previousValue = 200;
    relays.OutputList[4].httpEnabled = false;
    relays.OutputList[5].Enabled = false;
    relays.OutputList[6].GpioId = -1;
    pwmLevels[2] = 123;
    pwmLevels[3] = 200;
    digitalLevels[11] = 0;
    relays.SetTestModePaused(true);
    assert(relays.TestModePaused);
    assert(digitalLevels[10] == 0 && digitalLevels[11] == 1);
    assert(pwmLevels[2] == 0 && pwmLevels[3] == 255);
    assert(relays.OutputList[4].previousValue == 1);
    assert(relays.OutputList[5].previousValue == 1);
    assert(relays.OutputList[6].previousValue == 1);
    const uint32_t firstPauseWrites = writes;
    relays.SetTestModePaused(true);
    assert(writes == firstPauseWrites); // Applying a test retains the original snapshot.
    relays.SetTestModePaused(false);
    assert(!relays.TestModePaused);
    assert(digitalLevels[10] == 1 && digitalLevels[11] == 0);
    assert(pwmLevels[2] == 123 && pwmLevels[3] == 200);
    assert(relays.OutputList[0].previousValue == 1);
    assert(relays.OutputList[1].previousValue == 0);
    assert(relays.OutputList[2].previousValue == 123);
    assert(relays.OutputList[3].previousValue == 200);
    const uint32_t restoredWrites = writes;
    relays.SetTestModePaused(false);
    assert(writes == restoredWrites);

    // External pause/reboot discards the snapshot while keeping relays off.
    relays.SetTestModePaused(true);
    relays.SetTestModePaused(false, false);
    assert(!relays.TestModePaused);
    assert(digitalLevels[10] == 0 && digitalLevels[11] == 1);
    assert(pwmLevels[2] == 0 && pwmLevels[3] == 255);
    relays.SetTestModePaused(true);
    relays.SetTestModePaused(false);
    assert(digitalLevels[10] == 0 && digitalLevels[11] == 1);
    assert(pwmLevels[2] == 0 && pwmLevels[3] == 255);
    std::cout << "Test Mode HTTP relay pause and exact raw-level restore regressions passed\n";
}
