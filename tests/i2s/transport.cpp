#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#define ARDUINO_ARCH_ESP32
#define CONFIG_IDF_TARGET_ESP32
#define SUPPORT_PARALLEL_I2S
#define IRAM_ATTR
#define STAILQ_ENTRY(type) struct { type* stqe_next; }
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4201)
#endif
#include "hardware_types.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
i2s_dev_t I2S0{}, I2S1{};

using gpio_num_t = int;
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 1,
    ESP_ERR_INVALID_ARG = 2, ESP_ERR_INVALID_SIZE = 3, ESP_ERR_NO_MEM = 4, ESP_ERR_TIMEOUT = 5;
constexpr int ESP_INTR_FLAG_IRAM = 1, ESP_INTR_FLAG_LEVEL1 = 2, ETS_I2S1_INTR_SOURCE = 33;
constexpr int PERIPH_I2S1_MODULE = 1, MALLOC_CAP_DMA = 8;
constexpr int SIG_GPIO_OUT_IDX = 256, I2S1O_DATA_OUT0_IDX = 166, GPIO_MODE_OUTPUT = 2, PIN_FUNC_GPIO = 2;
static int GPIO_PIN_MUX_REG[40];
static bool valid_output(int pin) {
    return pin >= 0 && pin <= 33 && pin != 20 && pin != 24 && (pin < 28 || pin > 31);
}
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) valid_output(pin)

struct PinState { int level = -1, signal = -1, direction = -1; unsigned touches = 0; };
static PinState pinStates[40];
static unsigned checks = 0, modulesEnabled = 0, modulesDisabled = 0, barriers = 0, delays = 0;
static int64_t clockUs = 0;
static bool failInterrupt = false, failAllocation = false;
static std::function<void()> onDelay;
static std::map<void*, size_t> allocations;
static void* latestAllocation = nullptr;
static std::vector<std::pair<uintptr_t, size_t>> dmaRegions;
static void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

static void touch(int pin) {
    if (!valid_output(pin)) throw std::runtime_error("Touched invalid or disconnected output pin");
    ++pinStates[pin].touches;
}
static int gpio_set_level(int pin, int level) { touch(pin); pinStates[pin].level = level; return 0; }
static int gpio_set_direction(int pin, int direction) { touch(pin); pinStates[pin].direction = direction; return 0; }
static void gpio_matrix_out(int pin, int signal, bool invert, bool invertEnable) {
    touch(pin);
    assert(!invert && !invertEnable);
    pinStates[pin].signal = signal;
}
static void PIN_FUNC_SELECT(int pin, int function) { touch(pin); assert(function == PIN_FUNC_GPIO); }
static void periph_module_enable(int module) { assert(module == PERIPH_I2S1_MODULE); ++modulesEnabled; }
static void periph_module_disable(int module) { assert(module == PERIPH_I2S1_MODULE); ++modulesDisabled; }
static void periph_module_reset(int module) {
    assert(module == PERIPH_I2S1_MODULE);
    std::memset(const_cast<i2s_dev_s*>(&I2S1), 0, sizeof(I2S1));
    I2S1.state.tx_idle = 1;
}
static void* heap_caps_calloc(size_t count, size_t size, int capabilities) {
    assert(capabilities == MALLOC_CAP_DMA);
    if (failAllocation) return nullptr;
    void* data = std::calloc(count, size);
    if (data) { allocations[data] = count * size; latestAllocation = data; }
    return data;
}
static void heap_caps_free(void* pointer) {
    if (!pointer) return;
    require(allocations.erase(pointer) == 1, "Transport freed memory it does not own");
    std::free(pointer);
}
static bool esp_ptr_dma_capable(const void* pointer) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    for (const auto& region : dmaRegions) {
        if (address >= region.first && address - region.first < region.second) return true;
    }
    return false;
}
static int64_t esp_timer_get_time() { return clockUs; }
static void vTaskDelay(unsigned ticks) {
    ++delays;
    clockUs += int64_t(ticks) * 1000;
    const auto callback = onDelay;
    if (callback) callback();
}
static void publication_barrier() { ++barriers; std::atomic_thread_fence(std::memory_order_seq_cst); }

struct FakeInterrupt { void (*handler)(void*) = nullptr; void* context = nullptr; bool enabled = false; };
using intr_handle_t = FakeInterrupt*;
static std::set<FakeInterrupt*> interruptHandles;
static esp_err_t esp_intr_alloc(int source, int flags, void (*handler)(void*), void* context, intr_handle_t* handle) {
    assert(source == ETS_I2S1_INTR_SOURCE && flags == (ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1));
    if (failInterrupt) return ESP_FAIL;
    *handle = new FakeInterrupt{handler, context, true};
    interruptHandles.insert(*handle);
    return ESP_OK;
}
static esp_err_t esp_intr_disable(intr_handle_t handle) { assert(interruptHandles.count(handle)); handle->enabled = false; return ESP_OK; }
static esp_err_t esp_intr_enable(intr_handle_t handle) { assert(interruptHandles.count(handle)); handle->enabled = true; return ESP_OK; }
static esp_err_t esp_intr_free(intr_handle_t handle) {
    assert(interruptHandles.erase(handle));
    delete handle;
    return ESP_OK;
}

#include "transport_header.inc"
#include "transport_methods.inc"

static const gpio_num_t octaPins[8] = {0, 1, 2, 3, 4, 5, 12, 13};
static void interrupt(uint32_t status) {
    require(interruptHandles.size() == 1, "Unexpected interrupt ownership");
    auto* handle = *interruptHandles.begin();
    require(handle->enabled, "Interrupt was disabled during an active transfer");
    I2S1.int_st.val = status;
    handle->handler(handle->context);
    I2S1.int_st.val = 0; // W1C MMIO acknowledgement at the hardware seam.
}

struct Frame {
    std::vector<uint32_t> words;
    explicit Frame(size_t bytes) : words(bytes / 4) {
        assert(!(bytes & 3));
        auto* memory = data();
        for (size_t i = 0; i + 4 < bytes; ++i) memory[i ^ 2] = uint8_t((i * 37 + 9) & 255);
        std::memset(memory + bytes - 4, 0, 4);
        dmaRegions.push_back({reinterpret_cast<uintptr_t>(memory), bytes});
    }
    uint8_t* data() { return reinterpret_cast<uint8_t*>(words.data()); }
    const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(words.data()); }
    size_t size() const { return words.size() * 4; }
};

// Independent DMA engine: follow descriptor links as hardware would, with a
// cycle bound and the documented I2S1 16-bit-half ordering at the wire boundary.
static std::vector<uint8_t> transmit_descriptor_chain(const Frame& frame) {
    const auto* first = static_cast<const lldesc_t*>(latestAllocation);
    require((reinterpret_cast<uintptr_t>(first) & 0xfffff) == I2S1.out_link.addr, "DMA started at the wrong descriptor address");
    std::set<const lldesc_t*> visited;
    std::vector<uint8_t> memoryBytes;
    const lldesc_t* descriptor = first;
    unsigned endFlags = 0;
    while (descriptor) {
        require(visited.insert(descriptor).second, "DMA chain loops instead of completing once");
        require(visited.size() <= frame.size() / 4 + 1, "DMA chain failed to terminate");
        require(descriptor->owner == 1, "DMA descriptor was not handed to hardware");
        require(descriptor->length > 0 && descriptor->length <= 4092 && !(descriptor->length & 3), "Invalid DMA word length");
        require(descriptor->size == descriptor->length, "DMA size and transfer length diverged");
        require(descriptor->offset == 0 && descriptor->sosf == 0, "Unexpected DMA subframe or offset");
        require(descriptor->buf == frame.data() + memoryBytes.size(), "DMA omitted or repeated payload bytes");
        if (descriptor->eof) { ++endFlags; require(descriptor->qe.stqe_next == nullptr, "EOF occurred before the last descriptor"); }
        else require(descriptor->qe.stqe_next != nullptr, "Final DMA descriptor lacks EOF");
        for (size_t i = 0; i < descriptor->length; ++i) memoryBytes.push_back(uint8_t(descriptor->buf[i]));
        descriptor = descriptor->qe.stqe_next;
    }
    require(endFlags == 1 && memoryBytes.size() == frame.size(), "DMA did not transmit exactly one complete frame");
    std::vector<uint8_t> wire(memoryBytes.size());
    constexpr size_t fifoOrder[4] = {2, 3, 0, 1};
    for (size_t base = 0; base < memoryBytes.size(); base += 4) {
        for (size_t i = 0; i < 4; ++i) wire[base + i] = memoryBytes[base + fifoOrder[i]];
    }
    for (size_t i = 0; i + 4 < wire.size(); ++i) require(wire[i] == uint8_t((i * 37 + 9) & 255), "Hardware byte ordering changed the logical sample stream");
    for (size_t i = wire.size() - 4; i < wire.size(); ++i) require(wire[i] == 0, "Final samples do not leave every lane low");
    return wire;
}

static void finished(c_ParallelI2sBus& bus, uint32_t event = uint32_t(1) << 12) {
    I2S1.state.tx_idle = 0;
    interrupt(event);
    require(bus.IsBusy(), "DMA EOF released the frame while FIFO samples remained");
    I2S1.state.tx_idle = 1;
    require(!bus.IsBusy(), "Physical TX completion did not release the frame");
}

int main() {
    try {
        for (int pin = 0; pin < 40; ++pin) GPIO_PIN_MUX_REG[pin] = pin;
        c_ParallelI2sBus bus;
        Frame frame(4092 * 2 + 12);
        constexpr size_t expectedOrder[4] = {2, 3, 0, 1};
        for (size_t base = 0; base < 64; base += 4) {
            for (size_t index = 0; index < 4; ++index) {
                require(c_ParallelI2sBus::DmaByteIndex(base + index) == base + expectedOrder[index],
                        "Public sample-order helper disagrees with the hardware FIFO");
            }
        }
        require(!bus.Send(frame.data(), frame.size()) && bus.LastError() == ESP_ERR_INVALID_STATE, "Uninitialized transport accepted a transfer");
        require(!bus.IsBusy(), "Uninitialized transport reported active DMA");
        require(!bus.Begin(nullptr), "Null pin array was accepted");
        const gpio_num_t noPins[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
        require(!bus.Begin(noPins), "Empty lane set was accepted");
        gpio_num_t bad[8];
        std::copy(std::begin(octaPins), std::end(octaPins), bad);
        bad[4] = 34;
        require(!bus.Begin(bad), "Input-only GPIO was accepted as a lane");
        bad[4] = bad[3];
        require(!bus.Begin(bad), "Two lanes claimed the same GPIO");
        failInterrupt = true;
        require(!bus.Begin(octaPins) && interruptHandles.empty() && modulesEnabled == modulesDisabled, "Interrupt allocation failure leaked peripheral ownership");
        failInterrupt = false;
        require(bus.Begin(octaPins), "Valid Dig Octa pins failed initialization");
        require(!bus.IsBusy() && I2S1.conf.tx_start == 0 && I2S1.int_ena.val == 0, "Initialization started transmission prematurely");
        require(I2S1.conf2.lcd_en && I2S1.conf2.lcd_tx_wrx2_en && !I2S1.conf2.lcd_tx_sdx2_en, "Incorrect eight-bit parallel mode");
        require(I2S1.fifo_conf.tx_fifo_mod_force_en && I2S1.fifo_conf.tx_fifo_mod == 1 && I2S1.conf_chan.tx_chan_mod == 1, "Incorrect I2S1 byte FIFO mode");
        require(I2S1.clkm_conf.clk_en && !I2S1.clkm_conf.clka_en, "Parallel output stole the Ethernet APLL");
        const double divisor = I2S1.clkm_conf.clkm_div_num + double(I2S1.clkm_conf.clkm_div_b) / I2S1.clkm_conf.clkm_div_a;
        const double samplesPerSecond = 160000000.0 / divisor / I2S1.sample_rate_conf.tx_bck_div_num;
        require(samplesPerSecond == 3200000 && I2S1.sample_rate_conf.tx_bits_mod == 8, "Output clock does not provide four samples per 800 kHz bit");
        require(I2S1.conf1.tx_stop_en && I2S1.lc_conf.out_eof_mode && I2S1.lc_conf.check_owner, "Missing FIFO completion/owner safeguards");
        c_ParallelI2sBus other;
        require(!other.Begin(octaPins), "Two buses simultaneously claimed I2S1");
        require(!bus.Send(frame.data(), frame.size() - 1), "Unaligned frame length was accepted");
        require(!bus.Send(frame.data() + 1, frame.size() - 4), "Unaligned DMA pointer was accepted");
        require(!bus.Send(frame.data(), frame.size() + 4), "Frame extending beyond DMA-capable storage was accepted");
        alignas(4) uint8_t ordinary[8]{};
        require(!bus.Send(ordinary, sizeof(ordinary)), "Non-DMA memory was accepted");
        frame.data()[frame.size() - 1] = 1;
        require(!bus.Send(frame.data(), frame.size()), "A high final sample was accepted");
        frame.data()[frame.size() - 1] = 0;

        failAllocation = true;
        require(!bus.Send(frame.data(), frame.size()) && bus.LastError() == ESP_ERR_NO_MEM && allocations.empty(), "Descriptor allocation failure started or leaked DMA");
        failAllocation = false;
        const auto original = frame.words;
        require(bus.Send(frame.data(), frame.size()), "Valid frame was rejected");
        require(barriers == 1 && I2S1.out_link.start && !I2S1.out_link.stop && I2S1.conf.tx_start, "DMA start/publication sequence was incomplete");
        require(bus.IsBusy(), "New DMA frame was released before EOF");
        transmit_descriptor_chain(frame);
        const void* activeDescriptors = latestAllocation;
        require(!bus.Send(frame.data(), frame.size()) && latestAllocation == activeDescriptors, "Concurrent Send replaced active descriptors");
        require(frame.words == original, "Transport modified the caller's frame samples");
        finished(bus);
        require(bus.Wait(0), "Completed DMA unexpectedly blocked or failed");

        // Descriptor-chain-end is another valid EOF indication, but never a
        // substitute for the physical FIFO becoming idle.
        require(bus.Send(frame.data(), frame.size()), "Completed bus could not send another frame");
        finished(bus, uint32_t(1) << 16);
        require(frame.words == original, "Repeated send changed sample data");

        require(bus.Send(frame.data(), frame.size()), "Error fixture could not start DMA");
        I2S1.state.tx_idle = 0;
        interrupt(uint32_t(1) << 14);
        require(bus.HadDmaError() && !bus.IsBusy() && bus.LastError() == ESP_FAIL, "Descriptor error did not propagate");
        require(!I2S1.conf.tx_start && I2S1.out_link.stop && !I2S1.int_ena.val, "Descriptor error left hardware running");
        require(!bus.Wait(0), "Hardware error was reported as successful completion");
        bus.Stop();
        for (int pin : octaPins) require(pinStates[pin].level == 0 && pinStates[pin].signal == SIG_GPIO_OUT_IDX, "Stop did not hold a registered lane low");

        require(bus.Send(frame.data(), frame.size()) && !bus.HadDmaError(), "Transport could not recover after Stop/error");
        I2S1.state.tx_idle = 0;
        const int64_t began = clockUs;
        require(!bus.Wait(3) && bus.LastError() == ESP_ERR_TIMEOUT && clockUs - began == 3000 && !bus.IsBusy(), "Stalled DMA did not stop at its finite deadline");
        require(delays == 3, "DMA wait spun without yielding between deadline checks");
        require(bus.Send(frame.data(), frame.size()), "Wait completion fixture could not start");
        I2S1.state.tx_idle = 0;
        onDelay = [&] { interrupt(uint32_t(1) << 12); I2S1.state.tx_idle = 1; onDelay = {}; };
        require(bus.Wait(10), "Wait did not observe asynchronous physical completion");
        bus.End();
        bus.End();
        require(allocations.empty() && interruptHandles.empty() && modulesEnabled == modulesDisabled, "End leaked transport resources");

        const auto untouched = pinStates[2];
        const gpio_num_t sparsePins[8] = {0,-1,-1,-1,4,-1,-1,13};
        require(bus.Begin(sparsePins), "Sparse lane set failed initialization");
        Frame small(16);
        require(bus.Send(small.data(), small.size()), "Sparse lane send failed");
        require(pinStates[0].signal == I2S1O_DATA_OUT0_IDX && pinStates[4].signal == I2S1O_DATA_OUT0_IDX + 4 && pinStates[13].signal == I2S1O_DATA_OUT0_IDX + 7, "Sparse lanes were renumbered instead of retaining bit positions");
        bus.Stop();
        bus.End();
        require(pinStates[2].touches == untouched.touches && pinStates[2].signal == untouched.signal, "Sparse bus changed an unrelated RMT pin");
        require(other.Begin(octaPins), "End did not release exclusive I2S ownership");
        other.End();
        require(allocations.empty() && interruptHandles.empty() && modulesEnabled == modulesDisabled, "Final transport resources were unbalanced");
        std::cout << "I2S production transport/MMIO regressions passed: " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
