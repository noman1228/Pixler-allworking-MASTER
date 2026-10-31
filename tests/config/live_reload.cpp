#define CONFIG_RUNTIME_FIXTURE
#define main flashFaultChecks
#include "host.cpp"
#undef main

static void put(const String& name, unsigned revision) {
    String data = String("{\"") + name + "\":{\"revision\":" + String(revision) + "}}";
    AsyncWebServerRequest request;
    request.path = String("/conf/") + name + ".json";
    webServer.body(&request, reinterpret_cast<uint8_t*>(data.data()), uint32_t(data.size()), 0, uint32_t(data.size()));
    require(request.response == 0, "Raw body failed before commit");
    webServer.complete(&request);
    require(request.response == 200, "Config commit failed");
}

int main() {
    try {
        WebMgr.Install();
        wallTime = 1793476800;
        testClock = 10000;
        c_OutputCommon driver;
        OutputMgr.OutputChannelDrivers[0] = {&driver, true, 30};
        const auto poll = [] { OutputMgr.Poll(); InputMgr.PollConfig(); InputMgr.Process(); };
        put("input_config", 1);
        put("output_config", 1);
        require(InputMgr.applications == 0 && OutputMgr.applications == 0, "Config applied inside HTTP callback");
        testClock += 3999;
        poll();
        require(InputMgr.applications == 0 && OutputMgr.applications == 0, "Config debounce boundary changed");
        ++testClock;
        poll();
        require(InputMgr.appliedRevision == 1 && OutputMgr.appliedRevision == 1, "Saved config did not apply through actual managers");
        require(!InputMgr.ConfigReload.IsDue(testClock, 4000) && !OutputMgr.ConfigReload.IsDue(testClock, 4000),
                "Applied config left its reload pending");
        require(!driver.paused, "Configured output did not resume transmission");

        // A normal traffic pause is separate from configuring the input driver.
        InputMgr.PauseProcessing = true;
        put("input_config", 2);
        put("output_config", 2);
        testClock += 4000;
        poll();
        require(InputMgr.appliedRevision == 2 && OutputMgr.appliedRevision == 2 && InputMgr.PauseProcessing,
                "A traffic pause blocked saved settings or was incorrectly cleared");
        InputMgr.PauseProcessing = false;

        OutputMgr.TestMode.active = true;
        OutputMgr.ServiceTestMode();
        put("input_config", 3);
        put("output_config", 3);
        require(OutputMgr.TestMode.active && InputMgr.TestModePaused, "HTTP callback directly changed test ownership");
        poll();
        require(!OutputMgr.TestMode.active && !InputMgr.TestModePaused, "Saved config did not release Test Mode on main loop");
        testClock += 4000;
        poll();
        require(InputMgr.appliedRevision == 3 && OutputMgr.appliedRevision == 3,
                "Saved config did not apply after Test Mode released outputs");

        put("input_config", 4);
        put("output_config", 4);
        testClock += 1000;
        put("input_config", 5);
        put("output_config", 5);
        testClock += 3000;
        wallTime = 0; // A browser/NTP clock adjustment must not change debounce.
        poll();
        require(InputMgr.appliedRevision == 3 && OutputMgr.appliedRevision == 3, "Queued saves ignored latest debounce time");
        testClock += 1000;
        wallTime = 2147483647;
        poll();
        require(InputMgr.appliedRevision == 5 && OutputMgr.appliedRevision == 5, "Queued saves did not apply latest settings after a clock jump");

        testClock = UINT32_MAX - 999;
        put("input_config", 6);
        put("output_config", 6);
        testClock += 3999;
        poll();
        require(InputMgr.appliedRevision == 5 && OutputMgr.appliedRevision == 5, "Millis rollover skipped debounce");
        ++testClock;
        poll();
        require(InputMgr.appliedRevision == 6 && OutputMgr.appliedRevision == 6, "Millis rollover lost a pending reload");

        InputMgr.FileExtractionPaused = true;
        put("input_config", 7);
        testClock += 4000;
        poll();
        require(InputMgr.appliedRevision == 6, "Config reload ignored file extraction ownership");
        InputMgr.FileExtractionPaused = false;
        poll();
        require(InputMgr.appliedRevision == 7, "Extraction pause discarded pending settings");

        OutputMgr.TestMode.active = true;
        put("config", 1);
        poll();
        require(!OutputMgr.TestMode.active, "Core config save failed to request safe Test Mode release");
        testClock += 4000;
        require(coreReload.TakeIfDue(testClock, 4000), "Core config scheduler did not use monotonic delay");

        // Deliver another complete HTTP save while the previous file's parsed
        // settings are being applied. Its generation must remain pending.
        put("input_config", 8);
        InputMgr.onApply = [] { put("input_config", 9); };
        testClock += 4000;
        poll();
        require(InputMgr.appliedRevision == 8 && InputMgr.ConfigReload.IsDue(testClock, 0),
                "A save arriving during configuration application was lost");
        testClock += 3999;
        poll();
        require(InputMgr.appliedRevision == 8, "Overlapping save ignored its own debounce time");
        ++testClock;
        poll();
        require(InputMgr.appliedRevision == 9 && !InputMgr.ConfigReload.IsDue(testClock, 0),
                "Latest overlapping save did not apply after debounce");
        std::cout << "Actual config-save scheduling fixture passed: " << checks << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
