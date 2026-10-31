# Test Mode regression checks

Run from the repository root:

```powershell
python tests/testmode/run.py
node tests/testmode/count-ui.cjs
node tools/tests/testmode-ui.cjs
node tools/tests/testmode-startup.cjs
```

The C++ tests require a C++17 compiler and the ArduinoJson headers installed by a PlatformIO build. The runner uses `CXX`, an available GCC/Clang compiler, or installed Visual Studio Build Tools on Windows. It compiles the production pattern header and extracts the current firmware methods and HTTP callback into hardware fixtures on every run. Coverage includes RGB/RGBW addressing, color correction, output selection, bounds, timing, ownership during concurrent normal traffic, independent pause reasons, stop/resume, reboot, and request validation. Reallocation checks reproduce buffer exhaustion after a port grows and verify rejected ports lose their stale storage, valid ports keep their mapping, shrinking recovers the layout, and buffer canaries remain intact. Relay checks cover normal and inverted digital/PWM off states, exact raw-level restoration, repeated Apply preserving the original snapshot, and snapshot discard during an external pause or reboot.

Temporal checks verify that solid colors and Count LEDs do not rewrite the
transmit buffer as timer intervals pass, across minimum/maximum brightness and
interval settings. Identical Apply requests preserve the buffer and animation
phase. Simulated background reads between individual pixel writes verify an RGB
cycle does not expose a blanket black clear before repainting. Animated patterns
still update pixels in place; these checks do not promise an atomic frame change.

The GUI tests use jQuery from the web sources and jsdom. To install the isolated test dependency when it is absent:

```powershell
npm install --no-save --prefix .pio/ui-verification jsdom@26.1.0
```

Count LEDs uses one configured output. Its position slider selects the first white LED from 1 through the last physical LED; the group-size slider selects 1–5 LEDs. The group stays steady and shrinks near the end without wrapping. Start once, then move either slider to update the test live. The display shows the lit range and the number of LEDs actually lit. Stop Test & Resume releases the outputs to normal operation.

GUI coverage includes first/last LED selection, unsaved controls during status polling, Start/Apply/Stop ordering, queued stop requests, connection errors, and empty output configurations. The counting suite covers slider bounds, output changes, endpoint shrinking, live update coalescing, duplicate events, stale responses, running-state restoration, connection failures, and Stop priority. The startup smoke test loads the complete GUI and its real prerequisite scripts, checking navigation, URL hash loading and the stop banner across tabs. These checks simulate hardware and network responses; LED wiring and physical output still need to be verified on a controller.
