Run `python tests/config/run.py` from the repository root. The runner uses an
installed C++17 compiler and the PlatformIO ArduinoJson headers. It extracts the
current flash file methods and complete `/conf` upload route on every run.

The storage fixture injects open, seek, short-write, ordering and rename faults
for raw JSON and multipart uploads. Checks ensure readers retain the previous
config through staging and failures, successful replacement triggers one reload,
failed callbacks cannot later commit, and flash config changes do not scan the
SD card. Both native SD and simulated `/sd/` list-refresh behavior are covered.
The fixture models LittleFS replacement rename; it does not emulate flash power
loss or validate a physical controller.

Run `python tests/config/input_reload.py` to check input reloads against an
in-progress main-loop FTP poll. The runner extracts the production
`ProcessJsonConfig` method on every run. It applies 32 revisions to both input
channels, including input-type changes, and verifies that the latest options
reach the drivers without restarting FTP from the input task. The service
fixture records lifecycle calls at the task boundary; it does not emulate the
network stack or physical SD card. An alternate source can be tested with
`--source path/to/InputMgr.cpp` to demonstrate the previous regression.

Run `python tests/config/rmt_startup.py` to check first-time RMT activation. The
runner extracts the real member defaults, inline task dispatch, `Begin`, and
`PauseOutput` methods. Controlled task hooks exercise publication before setup
finishes, protocol-gap setup after `Begin`, zero-size/remapped output buffers,
and the manager's final resume. Transmission must stay stopped until that
resume, then start normally; repeated pause/resume and rejected buffer
allocations are also checked. Hardware boundaries are stubbed. An alternate
header can be checked with `--header path/to/OutputRmt.hpp`.

Run `python tests/config/live_reload.py` for the full save and reload sequence.
It executes the production HTTP route, filename dispatch, scheduling methods,
flash JSON reader, input configuration poll, and output poll/load methods. It
checks that changes apply on the main-loop path with Test Mode stopped or active,
while normal input traffic is paused, after a wall-clock adjustment or millis
rollover, and when several saves arrive close together or during another reload.
Extraction keeps its separate pause reason. The output fixture requires existing
transmitters to be paused before driver configuration changes, then resumed after
the new output mapping is ready. Physical LED timing remains a hardware check.
