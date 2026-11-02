# Parallel I2S for Dig-Octa

The `jmt_esp32_dig_octa_i2s` environment enables an optional parallel I2S output
backend for the original ESP32 on the Dig-Octa. The normal
`jmt_esp32_dig_octa` environment continues to use the existing output drivers.
The backend is selected when compiling the firmware; the web interface does
not add a transport selector.

## Build

From the repository root:

```powershell
pio run -e jmt_esp32_dig_octa_i2s
```

This environment inherits the Dig-Octa board, dependencies, build flags and
unflags, then adds `SUPPORT_PARALLEL_I2S`. It does not change the default build
environment or require editing `platformio_user.ini`. Local overrides in that
file still apply through the parent environment.

To build the existing RMT variant:

```powershell
pio run -e jmt_esp32_dig_octa
```

These commands compile firmware. They do not upload it to a controller.

## Supported outputs and configuration

In the optional build, WS2811 outputs on the eight Dig-Octa pixel ports use
the shared parallel transmitter. Existing output channel numbers, protocol
IDs, saved configuration files and web controls retain their meaning. Choose
WS2811 and configure pixel count, brightness, color order and other pixel
settings as usual.

The parallel backend shares frame transmission across its enabled lanes.
Different lane lengths are padded low after their data ends; the frame must
allow the longest configured lane to finish and meet its reset interval. A
complete DMA frame remains immutable while transmitting. The backend uses a
single DMA waveform buffer and rebuilds it only after transmission is idle.

Monster Permanent and other protocols continue to use their existing drivers,
including RMT where applicable. Monster Permanent has different pulse widths
and six-byte wire records; the initial parallel backend does not support it.
A port belongs to one transmitter at a time. Switching its protocol,
disabling it or reloading configuration must stop its previous transmitter
before the pin and output storage are reused. A parallel lane must never also
be driven by RMT.

This build targets the original ESP32 Dig-Octa pin mapping. It does not enable
parallel I2S on the JMT Wi-Fi or Ethernet boards, ESP8266, ESP32-S2, ESP32-S3 or
ESP32-C3. It reserves the I2S peripheral used by the backend; another feature
cannot use that same peripheral concurrently.

The transmitter reserves I2S1 and uses the 160 MHz peripheral PLL for a 3.2 MHz
sample clock. It does not retune the APLL used by the Dig-Octa Ethernet clock.

Existing Dig-Octa pin sharing still applies: output 8 uses GPIO13, which is also
the default I2C clock pin. Disable output 8 when using that I2C connection.

## DMA memory and configuration limits

The DMA waveform requires internal memory that the ESP32 DMA controller can
access. Free heap and PSRAM capacity alone do not establish that enough
suitable memory is available. The longest encoded WS2811 lane determines the
shared waveform length, so growing one lane can increase memory use for the
whole parallel frame. Null pixels, intensity bytes per pixel and reset padding
also contribute. The single-buffer design avoids a second complete DMA frame;
the coordinator still needs a stable source snapshot while encoding.

The current encoder stores four one-byte lane-mask samples per data bit, or
32 DMA bytes per serialized WS2811 byte. The shared waveform therefore needs
`32 × longest lane byte count`, plus an all-low reset tail of at least 350
microseconds (1,120 bytes at 3.2 MHz). A single 1,000-pixel RGB lane, with no
null pixels and the minimum reset interval, needs 97,120 waveform bytes before
DMA descriptors and the source snapshot are counted. Eight equal-length lanes
share that waveform length; they do not require eight separate waveforms.

The existing logical output-buffer limits still apply. The parallel backend
adds a separate DMA memory requirement; it does not increase the configured
channel capacity or promise a fixed maximum number of pixels per port.

If sizing, DMA allocation or transmitter initialization fails, the parallel
outputs must remain stopped and report the failure. The firmware must not
silently start RMT on the same pins as a fallback. Reduce the configured output
length or use the normal Dig-Octa firmware to choose RMT explicitly.

## Validation

Host regressions belong in `tests/i2s/`. They exercise encoding and transport
ownership without operating a controller. Firmware builds check compatibility
with the installed ESP32 toolchain and headers. Neither establishes physical
pulse timing, DMA behavior or wiring correctness.

Before treating the optional backend as hardware-verified, check the controller
with a logic analyzer and representative LEDs. Cover first activation from
Disabled, simultaneous use of all eight lanes, unequal lane lengths, WS2811
settings, transitions between WS2811 and RMT-backed protocols such as Monster
Permanent, configuration reloads, pause/resume and allocation refusal. Confirm
pulse widths and reset intervals
at the actual output pins under network and playback load. Record the board,
firmware build, configured lengths and measured timings with those results.

No physical upload or hardware validation is performed by the build commands
above.
