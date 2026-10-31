#pragma once
#include <ctime>
#include "DeferredConfigReload.hpp"

static time_t wallTime = 0;
time_t now() { return wallTime; }
static DeferredConfigReload coreReload;
void ScheduleLoadConfig();
#define LOAD_CONFIG_DELAY 4
#define INPUT_STATE_GUARD
#define OUTPUT_DATA_GUARD
static bool IsBooting = false;
static constexpr SeekMode SeekSet = SeekMode::SeekSet;
static const char* MN_15 = "Could not load output configuration";
static struct { unsigned BlankDelay = 0; } config;

struct c_InputCommon {
    void SetBlankTimerIsRunning(bool) {}
    void Process() {}
    unsigned GetInputChannelId() const { return 0; }
};
struct c_InputMgr {
    struct Driver { c_InputCommon* InputDriver = nullptr; bool DriverInUse = false; };
    Driver InputChannelDrivers[2];
    struct { void Poll() {} } ExternalInput;
    static constexpr unsigned InputSecondaryChannelId = 1;
    bool PauseProcessing = false, TestModePaused = false, FileExtractionPaused = false;
    bool configInProgress = false, RebootNeeded = false;
    String InputRebootReason;
    char ConfigFileName[65] = "/input_config.json";
    DeferredConfigReload ConfigReload;
    unsigned appliedRevision = 0, applications = 0;
    std::function<void()> onApply;
    void ScheduleLoadConfig();
    void Process();
    void PollConfig();
    void LoadConfig();
    bool BlankTimerHasExpired(unsigned) const { return true; }
    void RestartBlankTimer(unsigned) {}
    void ApplyOperationalState() {}
    void SetTestModePaused(bool paused) { TestModePaused = paused; }
    void CreateNewConfig() { throw std::runtime_error("Unexpected input defaults"); }
    void ProcessJsonConfig(JsonObject& document) {
        appliedRevision = document["input_config"]["revision"].as<unsigned>();
        ++applications;
        auto callback = std::move(onApply);
        onApply = {};
        if (callback) { callback(); }
    }
} InputMgr;

struct c_OutputCommon {
    bool paused = false;
    void PauseOutput(bool value) { paused = value; }
    void Poll() {}
};
struct c_OutputMgr {
    struct DriverInfo_t { c_OutputCommon* OutputDriver = nullptr; bool OutputDriverInUse = false; unsigned OutputBufferDataSize = 0; };
    DriverInfo_t OutputChannelDrivers[1];
    struct { bool active = false; } TestMode;
    bool ConfigInProgress = false, OutputIsPaused = false, InputTestPauseApplied = false;
    char ConfigFileName[65] = "/output_config.json";
    DeferredConfigReload ConfigReload;
    std::atomic<bool> ConfigReloadStopRequested{false};
    unsigned appliedRevision = 0, applications = 0;
    void ScheduleLoadConfig();
    void Poll();
    void LoadConfig();
    void ServiceTestMode();
    void RequestConfigReloadStop() { ConfigReloadStopRequested.store(true, std::memory_order_release); }
    void ClearBuffer() {}
    void RenderTestMode() {}
    void StopTestMode(bool = true) { TestMode.active = false; }
    void CreateNewConfig() { throw std::runtime_error("Unexpected output defaults"); }
    void ProcessJsonConfig(JsonDocument& document) {
        for (const auto& output : OutputChannelDrivers) {
            require(!output.OutputDriverInUse || output.OutputDriver->paused, "Output driver was reconfigured while transmitting");
        }
        appliedRevision = document["output_config"]["revision"].as<unsigned>();
        ++applications;
    }
} OutputMgr;

#include "live_methods.inc"
