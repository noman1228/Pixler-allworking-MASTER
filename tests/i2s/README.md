Run `python tests/i2s/run.py` from the repository root. A C++ compiler is
required; the runner can initialize Visual Studio Build Tools on Windows.
The production encoder is portable C++11 and has no Arduino dependencies.

`ParallelI2sEncoder::Lane` supplies serialized bytes, byte count, and the lane's
required reset gap in microseconds. `GetLayout(lanes, layout)` checks sizes and
reports payload, reset, and total destination lengths. `Encode(lanes, buffer,
capacity, written)` validates before writing. Its destination must not overlap
the lane payloads. Empty inputs produce an all-low reset-only frame.

Each chronological sample byte contains lane 0 in bit 0 through lane 7 in bit 7.
At 3.2 MHz, each MSB-first source bit becomes four samples: `1000` for zero and
`1110` for one. A finished lane stays low while longer lanes continue; no extra
zero data bits or end-of-frame data bits are emitted. The all-low reset tail
meets the largest requested gap with a minimum of 350 microseconds, rounded up
to four-byte alignment. Hardware DMA ordering is the transport's responsibility.

The tests independently decode pulse widths and compare every source byte.
They cover golden masks, every possible byte on each lane, unequal and empty
lanes, 400 deterministic randomized frames, reset timing, output canaries,
undersized/null destinations, invalid sources, and size arithmetic overflow.
They do not validate the ESP32 I2S peripheral or physical output timing.

Run `python tests/i2s/transport_factory.py` to compile the production output
factory with the optional build flag both enabled and disabled. Hardware driver
seams record the selected transmitter. The cases cover all eight Dig-Octa pixel
ports, a UART port, transitions through Disabled, unchanged configuration,
WS2811 versus Monster Permanent, and preservation of saved protocol/channel IDs.

Run `python tests/i2s/ownership.py` for the production parallel-frame coordinator.
The runner extracts its class and methods on every run, using the production
encoder with a fake DMA peripheral and injected allocator failures. Tests check
that an active DMA frame and snapshot remain immutable, busy transfers do not
advance pixel serializers, DMA byte ordering decodes correctly on unequal lanes,
unregistered lanes relinquish their pins, cleanup stops DMA before releasing
storage, and initialization/snapshot/allocation/send/DMA/timeout failures stop
the outputs and can recover after a configuration reload.

Run `python tests/i2s/transport.py` for the actual low-level transport methods
against the installed ESP32 register layouts and a modeled DMA/GPIO/interrupt
boundary. It checks descriptor termination, FIFO byte order, EOF versus physical
completion, timeouts, error recovery, allocation failure, pin isolation and the
PLL clock settings. These checks do not establish electrical output timing.

Run `python tests/i2s/pixel_snapshot.py` for the existing pixel serializer's
production snapshot helpers and state machine. Coverage includes RGB/RGBW
prefixes, suffixes, null pixels, partial pixel lengths, capacity canaries,
invalid pointers, zero buffers and repeated snapshots. It also checks Monster
Permanent's six-byte expansion and color correction as a shared serializer
regression; Monster Permanent remains on its existing transport in this build.
