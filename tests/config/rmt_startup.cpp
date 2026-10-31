#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

using String = std::string;
using gpio_num_t = int;
using rmt_channel_t = unsigned;
using TaskHandle_t = void*;
using rmt_isr_handle_t = void*;
#define F(value) value
#define DEBUG_START
#define DEBUG_END
#define IRAM_ATTR
#define ESP_ERROR_CHECK(value) require((value) == 0, "Unexpected hardware configuration failure")
static constexpr unsigned ESP_INTR_FLAG_IRAM = 1, ESP_INTR_FLAG_LEVEL1 = 2, ESP_INTR_FLAG_SHARED = 4;
static constexpr unsigned RMT_Clock_Divisor = 2;

static unsigned checks = 0;
static void require(bool success, const char* message)
{
    ++checks;
    if (!success) { throw std::runtime_error(message); }
}

enum class rmt_mode_t { RMT_MODE_TX };
enum class rmt_reserve_memsize_t { RMT_MEM_64 };
enum class rmt_carrier_level_t { RMT_CARRIER_LEVEL_LOW };
struct rmt_config_t
{
    rmt_mode_t rmt_mode{};
    rmt_channel_t channel = 0;
    gpio_num_t gpio_num = 0;
    unsigned clk_div = 0;
    rmt_reserve_memsize_t mem_block_num{};
    struct
    {
        unsigned carrier_freq_hz = 0, carrier_duty_percent = 0, idle_level = 0;
        rmt_carrier_level_t carrier_level{};
        bool carrier_en = false, loop_en = false, idle_output_en = false;
    } tx_config;
};
static rmt_config_t RMT_DEFAULT_CONFIG_TX(gpio_num_t, rmt_channel_t) { return {}; }
static void ResetGpio(gpio_num_t) {}
static void RequestReboot(String&, unsigned) { throw std::runtime_error("Unexpected reboot request"); }
static int rmt_config(rmt_config_t*) { return 0; }
static void rmt_intr_handler(void*) {}
static int rmt_isr_register(void (*)(void*), void*, unsigned, rmt_isr_handle_t* handle)
{
    *handle = reinterpret_cast<void*>(1);
    return 0;
}
static void RMT_Task(void*) {}
static std::function<void()> taskCreationHook;
static void xTaskCreatePinnedToCore(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t* handle, unsigned)
{
    *handle = reinterpret_cast<void*>(1);
    if (taskCreationHook) { taskCreationHook(); }
}
static void vTaskPrioritySet(TaskHandle_t, unsigned) {}

// The transmitter seam records a frame start, independently of the driver's
// pause/initialization checks. It makes premature dispatch observable without
// emulating the ESP32 peripheral or depending on an actual scheduler race.
struct c_OutputCommon
{
    bool protocolReady = false, bufferMapped = false;
    unsigned bufferSize = 0, frameStarts = 0, invalidFrameStarts = 0;
    bool RmtPoll()
    {
        ++frameStarts;
        if (!protocolReady || !bufferMapped || !bufferSize) { ++invalidFrameStarts; }
        return true;
    }
};

class c_OutputRmt
{
public:
    struct OutputRmtConfig_t
    {
        enum DataDirection_t { MSB2LSB, LSB2MSB };
        unsigned IntensityDataWidth = 8, idle_level = 0;
        bool SendInterIntensityBits = false;
        DataDirection_t DataDirection = MSB2LSB;
        rmt_channel_t RmtChannelId = 0;
        gpio_num_t DataPin = 4;
        c_OutputCommon* pPixelDataSource = nullptr;
        const unsigned* CitrdsArray = nullptr;
    } OutputRmtConfig;

    // These declarations and the inline dispatch method come from the real
    // header on every run, preserving the production defaults in this test.
#include "rmt_startup_members.inc"
    c_OutputCommon* pParent = nullptr;
    unsigned NumRmtSlotsPerIntensityValue = 0, TxIntensityDataStartingMask = 0;
    unsigned resets = 0, translations = 0, interruptDisables = 0, interruptClears = 0;
    void Begin(OutputRmtConfig_t config, c_OutputCommon* parent);
    void PauseOutput(bool);
    void ISR_ResetRmtBlockPointers() { ++resets; }
    void UpdateBitXlatTable(const unsigned*) { ++translations; }
    void DisableRmtInterrupts() { ++interruptDisables; }
    void ClearRmtInterrupts() { ++interruptClears; }
};

static std::function<void(c_OutputRmt*)> publicationHook;
struct PublishedSlot
{
    c_OutputRmt* pointer = nullptr;
    PublishedSlot& operator=(c_OutputRmt* value)
    {
        pointer = value;
        if (value && publicationHook) { publicationHook(value); }
        return *this;
    }
};
static std::array<PublishedSlot, 8> rmt_isr_ThisPtrs;
static rmt_isr_handle_t RMT_intr_handle = nullptr;
static TaskHandle_t SendFrameTaskHandle = nullptr;

#include "rmt_startup_methods.inc"

static bool pollPublished(unsigned channel)
{
    auto* driver = rmt_isr_ThisPtrs[channel].pointer;
    return driver ? driver->StartNextFrame() : false;
}

int main()
{
    try
    {
        // Even an out-of-order resume must not dispatch an uninitialized
        // instance with a parent. This independently checks the init guard.
        c_OutputCommon unreadyParent;
        c_OutputRmt unready;
        unready.pParent = &unreadyParent;
        unready.PauseOutput(false);
        require(!unready.StartNextFrame() && unreadyParent.frameStarts == 0,
                "Uninitialized RMT instance dispatched a frame");

        c_OutputCommon parent;
        c_OutputRmt driver;
        c_OutputRmt::OutputRmtConfig_t config;
        config.pPixelDataSource = &parent;
        const unsigned baseTranslations = 1;
        config.CitrdsArray = &baseTranslations;
        unsigned earlyTaskPolls = 0;
        taskCreationHook = [&] {
            ++earlyTaskPolls;
            require(!driver.StartNextFrame() && parent.frameStarts == 0,
                    "Freshly created task transmitted before driver publication");
        };
        publicationHook = [&](c_OutputRmt* published) {
            ++earlyTaskPolls;
            require(!published->StartNextFrame() && parent.frameStarts == 0,
                    "Published RMT instance transmitted before initialization completed");
        };
        driver.Begin(config, &parent);
        require(earlyTaskPolls == 2 && driver.translations == 1,
                "Startup fixture did not exercise the production task/publication boundaries");
        require(!pollPublished(0) && parent.frameStarts == 0,
                "RMT transmitted before the protocol-specific gap was configured");

        // WS2811 finishes its gap lookup after Rmt.Begin returns. The manager
        // then clears old allocations, maps the new buffer and finally resumes.
        parent.protocolReady = true;
        require(!pollPublished(0), "RMT transmitted before any output buffer was assigned");
        driver.PauseOutput(true);
        parent.bufferMapped = true;
        parent.bufferSize = 0;
        require(!pollPublished(0), "RMT transmitted during the zero-length remap stage");
        parent.bufferSize = 300;
        require(!pollPublished(0), "RMT transmitted before the manager resumed the completed mapping");
        driver.PauseOutput(false);
        require(pollPublished(0) && parent.frameStarts == 1 && parent.invalidFrameStarts == 0,
                "Completed RMT output did not begin sending after manager resume");

        driver.PauseOutput(true);
        const unsigned disables = driver.interruptDisables, clears = driver.interruptClears;
        require(disables == 1 && clears == 1 && !pollPublished(0),
                "Pausing an active RMT output did not stop dispatch and disable interrupts");
        driver.PauseOutput(true);
        require(driver.interruptDisables == disables && driver.interruptClears == clears,
                "Repeated RMT pause unnecessarily changed peripheral state");
        driver.PauseOutput(false);
        require(pollPublished(0) && parent.frameStarts == 2,
                "Previously paused RMT output did not resume");
        driver.PauseOutput(true);
        parent.bufferSize = 0;
        require(!pollPublished(0) && parent.frameStarts == 2 && parent.invalidFrameStarts == 0,
                "An output rejected during buffer allocation resumed transmission");

        std::cout << "RMT startup/pause regression passed (" << checks << " checks).\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
