/*
 * One-shot parallel I2S1 transport for the original ESP32.
 *
 * Register configuration informed by esp_i2s_parallel:
 * https://github.com/TobleMiner/esp_i2s_parallel
 * Copyright (c) 2019 Tobias Schramm
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "output/ParallelI2sBus.hpp"

#if defined(ARDUINO_ARCH_ESP32) && defined(SUPPORT_PARALLEL_I2S) && defined(CONFIG_IDF_TARGET_ESP32)

#include <string.h>
#include <driver/periph_ctrl.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp32/rom/gpio.h>
#include <soc/gpio_sig_map.h>
#include <soc/i2s_struct.h>
#include <soc/io_mux_reg.h>
#include <soc/soc_memory_types.h>

namespace
{
    // DMA descriptors have a 12-bit byte count. Align every descriptor to a
    // complete 32-bit FIFO word to preserve sample ordering at boundaries.
    constexpr size_t MaxDescriptorBytes = 4092;
    constexpr uint32_t DmaEofInterrupt = uint32_t(1) << 12;
    constexpr uint32_t DmaErrorInterrupt = uint32_t(1) << 14;
    constexpr uint32_t DmaChainEndInterrupt = uint32_t(1) << 16;
    c_ParallelI2sBus* ActiveBus = nullptr;

    void ResetTransmitPath()
    {
        I2S1.lc_conf.out_rst = 1;
        I2S1.lc_conf.out_rst = 0;
        I2S1.lc_conf.ahbm_rst = 1;
        I2S1.lc_conf.ahbm_rst = 0;
        I2S1.lc_conf.ahbm_fifo_rst = 1;
        I2S1.lc_conf.ahbm_fifo_rst = 0;
        I2S1.conf.tx_reset = 1;
        I2S1.conf.tx_reset = 0;
        I2S1.conf.tx_fifo_reset = 1;
        I2S1.conf.tx_fifo_reset = 0;
    }
}

c_ParallelI2sBus::~c_ParallelI2sBus()
{
    End();
}

bool c_ParallelI2sBus::Begin(const gpio_num_t pins[LaneCount])
{
    if (!pins || (ActiveBus && ActiveBus != this))
    {
        Error = ESP_ERR_INVALID_STATE;
        return false;
    }
    bool anyPin = false;
    for (size_t lane = 0; lane < LaneCount; ++lane)
    {
        if (pins[lane] == gpio_num_t(-1)) { continue; }
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pins[lane]))
        {
            Error = ESP_ERR_INVALID_ARG;
            return false;
        }
        for (size_t previous = 0; previous < lane; ++previous)
        {
            if (pins[previous] == pins[lane])
            {
                Error = ESP_ERR_INVALID_ARG;
                return false;
            }
        }
        anyPin = true;
    }
    if (!anyPin)
    {
        Error = ESP_ERR_INVALID_ARG;
        return false;
    }

    End();
    memcpy(Pins, pins, sizeof(Pins));
    ActiveBus = this;
    periph_module_enable(PERIPH_I2S1_MODULE);
    periph_module_reset(PERIPH_I2S1_MODULE);
    Initialized = true;
    HoldPinsLow();
    HaltHardware();
    ResetTransmitPath();

    // Eight-bit LCD mode on I2S1 emits all eight data bits simultaneously.
    // No external clock pin is needed by one-wire pixels.
    I2S1.conf.val = 0;
    I2S1.conf.tx_right_first = 1;
    I2S1.conf2.val = 0;
    I2S1.conf2.lcd_en = 1;
    I2S1.conf2.lcd_tx_wrx2_en = 1;
    I2S1.conf2.lcd_tx_sdx2_en = 0;
    I2S1.fifo_conf.val = 0;
    I2S1.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S1.fifo_conf.tx_fifo_mod = 1;
    I2S1.fifo_conf.tx_data_num = 32;
    I2S1.fifo_conf.dscr_en = 1;
    I2S1.conf_chan.val = 0;
    I2S1.conf_chan.tx_chan_mod = 1;
    I2S1.conf1.val = 0;
    I2S1.conf1.tx_pcm_bypass = 1;
    I2S1.conf1.tx_stop_en = 1;
    I2S1.timing.val = 0;
    I2S1.pdm_conf.val = 0;

    // PLL_D2 (160 MHz), not the APLL used by Dig Octa Ethernet:
    // 160 MHz / (12 + 1/2) / 4 = 3.2 MHz, four samples per 800 kHz bit.
    I2S1.clkm_conf.val = 0;
    I2S1.clkm_conf.clk_en = 1;
    I2S1.clkm_conf.clka_en = 0;
    I2S1.clkm_conf.clkm_div_num = 12;
    I2S1.clkm_conf.clkm_div_a = 2;
    I2S1.clkm_conf.clkm_div_b = 1;
    I2S1.sample_rate_conf.val = 0;
    I2S1.sample_rate_conf.tx_bits_mod = 8;
    I2S1.sample_rate_conf.tx_bck_div_num = 4;
    I2S1.lc_conf.val = 0;
    // EOF must follow FIFO consumption, not merely the last memory fetch.
    I2S1.lc_conf.out_eof_mode = 1;
    I2S1.lc_conf.check_owner = 1;

    Error = esp_intr_alloc(ETS_I2S1_INTR_SOURCE,
                           ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1,
                           InterruptHandler, this, &Interrupt);
    if (Error != ESP_OK)
    {
        const esp_err_t failure = Error;
        End();
        Error = failure;
        return false;
    }
    Busy = DmaEof = DmaError = false;
    Error = ESP_OK;
    return true;
}

bool c_ParallelI2sBus::Send(const uint8_t* dmaSamples, size_t bytes)
{
    if (!Initialized || IsBusy())
    {
        Error = ESP_ERR_INVALID_STATE;
        return false;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(dmaSamples);
    if (!dmaSamples || bytes < 4 || (bytes & 3) || (address & 3) ||
        bytes - 1 > UINTPTR_MAX - address || !esp_ptr_dma_capable(dmaSamples) ||
        !esp_ptr_dma_capable(reinterpret_cast<const void*>(address + bytes - 1)))
    {
        Error = ESP_ERR_INVALID_ARG;
        return false;
    }
    // The last word must be low so stopping the bus cannot leave a lane high.
    for (size_t index = bytes - 4; index < bytes; ++index)
    {
        if (dmaSamples[index])
        {
            Error = ESP_ERR_INVALID_ARG;
            return false;
        }
    }
    const size_t count = 1 + (bytes - 1) / MaxDescriptorBytes;
    if (count > SIZE_MAX / sizeof(lldesc_t))
    {
        Error = ESP_ERR_INVALID_SIZE;
        return false;
    }
    if (count > DescriptorCapacity)
    {
        auto* replacement = static_cast<lldesc_t*>(heap_caps_calloc(count, sizeof(lldesc_t), MALLOC_CAP_DMA));
        if (!replacement)
        {
            Error = ESP_ERR_NO_MEM;
            return false;
        }
        heap_caps_free(Descriptors);
        Descriptors = replacement;
        DescriptorCapacity = count;
    }
    size_t offset = 0;
    for (size_t index = 0; index < count; ++index)
    {
        const size_t remaining = bytes - offset;
        const size_t length = remaining < MaxDescriptorBytes ? remaining : MaxDescriptorBytes;
        lldesc_t& descriptor = Descriptors[index];
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.size = length;
        descriptor.length = length;
        descriptor.owner = 1;
        descriptor.eof = index + 1 == count;
        descriptor.buf = const_cast<uint8_t*>(dmaSamples + offset);
        descriptor.qe.stqe_next = index + 1 < count ? &Descriptors[index + 1] : nullptr;
        offset += length;
    }

    esp_intr_disable(Interrupt);
    HaltHardware();
    ResetTransmitPath();
    DmaEof = DmaError = false;
    Error = ESP_OK;
    AttachPins();
    // Publish descriptors before enabling DMA; the Xtensa memory barrier also
    // orders caller sample writes before peripheral reads.
    __asm__ __volatile__("memw" ::: "memory");
    Busy = true;
    I2S1.out_link.val = 0;
    I2S1.out_link.addr = reinterpret_cast<uintptr_t>(Descriptors);
    I2S1.int_clr.val = UINT32_MAX;
    I2S1.int_ena.val = DmaEofInterrupt | DmaChainEndInterrupt | DmaErrorInterrupt;
    I2S1.out_link.start = 1;
    I2S1.conf.tx_start = 1;
    esp_intr_enable(Interrupt);
    return true;
}

bool c_ParallelI2sBus::IsBusy() const
{
    if (!Initialized || !Busy || DmaError) { return false; }
    // The DMA chain ending alone is insufficient: samples may remain in the
    // FIFO. The hardware stop-on-empty flag makes tx_idle the final boundary.
    return !DmaEof || !I2S1.state.tx_idle;
}

bool c_ParallelI2sBus::Wait(uint32_t timeoutMs)
{
    const int64_t deadline = esp_timer_get_time() + int64_t(timeoutMs) * 1000;
    while (IsBusy())
    {
        if (esp_timer_get_time() >= deadline)
        {
            Stop();
            Error = ESP_ERR_TIMEOUT;
            return false;
        }
        vTaskDelay(1);
    }
    return !DmaError;
}

void IRAM_ATTR c_ParallelI2sBus::HaltHardware()
{
    I2S1.int_ena.val = 0;
    I2S1.out_link.stop = 1;
    I2S1.out_link.start = 0;
    I2S1.conf.tx_start = 0;
    I2S1.lc_conf.out_rst = 1;
    I2S1.lc_conf.out_rst = 0;
    I2S1.conf.tx_fifo_reset = 1;
    I2S1.conf.tx_fifo_reset = 0;
    I2S1.int_clr.val = UINT32_MAX;
}

void IRAM_ATTR c_ParallelI2sBus::InterruptHandler(void* argument)
{
    auto* bus = static_cast<c_ParallelI2sBus*>(argument);
    const uint32_t status = I2S1.int_st.val;
    I2S1.int_clr.val = status;
    if (status & DmaErrorInterrupt)
    {
        HaltHardware();
        bus->Error = ESP_FAIL;
        bus->DmaError = true;
        bus->Busy = false;
    }
    else if (status & (DmaEofInterrupt | DmaChainEndInterrupt))
    {
        bus->DmaEof = true;
        I2S1.int_ena.val = 0;
    }
}

void c_ParallelI2sBus::HoldPinsLow()
{
    for (gpio_num_t pin : Pins)
    {
        if (pin == gpio_num_t(-1)) { continue; }
        // Set the GPIO latch before disconnecting the peripheral to avoid a
        // high pulse while handing ownership back from I2S.
        gpio_set_level(pin, 0);
        gpio_matrix_out(pin, SIG_GPIO_OUT_IDX, false, false);
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    }
    PinsAttached = false;
}

void c_ParallelI2sBus::AttachPins()
{
    if (PinsAttached) { return; }
    for (size_t lane = 0; lane < LaneCount; ++lane)
    {
        const gpio_num_t pin = Pins[lane];
        if (pin == gpio_num_t(-1)) { continue; }
        gpio_set_level(pin, 0);
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_matrix_out(pin, I2S1O_DATA_OUT0_IDX + lane, false, false);
    }
    PinsAttached = true;
}

void c_ParallelI2sBus::Stop()
{
    if (!Initialized) { return; }
    if (Interrupt) { esp_intr_disable(Interrupt); }
    HaltHardware();
    Busy = false;
    DmaEof = false;
    HoldPinsLow();
}

void c_ParallelI2sBus::End()
{
    if (!Initialized) { return; }
    Stop();
    if (Interrupt)
    {
        esp_intr_free(Interrupt);
        Interrupt = nullptr;
    }
    heap_caps_free(Descriptors);
    Descriptors = nullptr;
    DescriptorCapacity = 0;
    periph_module_disable(PERIPH_I2S1_MODULE);
    Initialized = false;
    if (ActiveBus == this) { ActiveBus = nullptr; }
}

#endif // classic ESP32 && SUPPORT_PARALLEL_I2S
