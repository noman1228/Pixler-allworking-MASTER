#include "ESPixelStick.h"
#include <atomic>
#include <future>
#include <thread>

static constexpr const char* CN_type = "type";
static constexpr const char* MN_19 = "Invalid type ";
static constexpr const char* MN_20 = " on channel ";
static constexpr const char* MN_03 = ".";
static unsigned checks = 0;

inline String operator+(const String& left, unsigned right)
{
    return String(static_cast<const std::string&>(left) + std::to_string(right));
}

inline String operator+(const String& left, const char* right)
{
    return String(static_cast<const std::string&>(left) + right);
}

static void require(bool success, const char* message)
{
    ++checks;
    if (!success) { throw std::runtime_error(message); }
}

// FTP belongs to the main-loop service. Hold an in-progress poll at a
// controllable boundary while the other task applies input configurations.
struct FtpService
{
    std::atomic<bool> polling{false};
    std::atomic<unsigned> lifecycleCalls{0}, concurrentLifecycleCalls{0};

    void handleFTP(std::promise<void>& entered, std::future<void>& release)
    {
        polling = true;
        entered.set_value();
        release.wait();
        polling = false;
    }

    void restart()
    {
        ++lifecycleCalls;
        if (polling.load()) { ++concurrentLifecycleCalls; }
    }
} ftp;

struct FileManager
{
    // Preserve the old API at the hardware seam so the regression fails on
    // the previous production method, rather than merely failing to compile.
    void UpdateFtp() { ftp.restart(); }
} FileMgr;

class c_InputCommon
{
public:
    unsigned type = 0, applications = 0, value = 0;
    unsigned GetInputType() const { return type; }
    void SetConfig(JsonObject& config)
    {
        value = config["value"].as<unsigned>();
        ++applications;
    }
};

class c_InputMgr
{
public:
    enum e_InputType
    {
        InputType_DDP, InputType_FPP, InputType_Effects,
        InputType_Disabled, InputType_End,
        InputType_Default = InputType_Disabled
    };
    enum e_InputChannelIds { InputChannelId_Start, InputChannelId_End = 2 };
    struct Driver
    {
        c_InputCommon* InputDriver = nullptr;
        bool DriverInUse = false;
    } InputChannelDrivers[InputChannelId_End];
    c_InputCommon drivers[InputChannelId_End];
    bool EffectEngineIsConfiguredToRun[InputChannelId_End] = {};
    unsigned fallbackConfigs = 0;

    bool ProcessJsonConfig(JsonObject&);
    bool FindJsonChannelConfig(JsonObject& config, e_InputChannelIds channel, JsonObject& result)
    {
        result = config["input_config"]["channels"][String(unsigned(channel))].as<JsonObject>();
        return !result.isNull();
    }
    void InstantiateNewInputChannel(e_InputChannelIds channel, e_InputType type)
    {
        drivers[channel].type = type;
        InputChannelDrivers[channel] = {&drivers[channel], true};
    }
    void CreateNewConfig() { ++fallbackConfigs; }
};

template<class T>
static bool setFromJSON(T& target, JsonObject& object, const char* name)
{
    if (object[name].isNull()) { return false; }
    target = object[name].as<T>();
    return true;
}

#include "input_reload_method.inc"

int main()
{
    try
    {
        std::promise<void> pollEntered, releasePoll;
        auto entered = pollEntered.get_future();
        auto release = releasePoll.get_future();
        std::thread mainLoop([&] { ftp.handleFTP(pollEntered, release); });
        entered.wait();

        c_InputMgr manager;
        bool allApplied = true;
        for (unsigned revision = 1; revision <= 32; ++revision)
        {
            JsonDocument document;
            JsonObject root = document.to<JsonObject>();
            JsonObject channels = root["input_config"]["channels"].to<JsonObject>();
            for (unsigned channel = 0; channel < 2; ++channel)
            {
                // Exercise both normal inputs and an input-type change.
                unsigned type = channel == 0 ? c_InputMgr::InputType_DDP :
                    (revision & 1 ? c_InputMgr::InputType_FPP : c_InputMgr::InputType_Effects);
                JsonObject input = channels[String(channel)].to<JsonObject>();
                input["type"] = type;
                input[String(type)]["value"] = revision * 10 + channel;
            }
            allApplied &= manager.ProcessJsonConfig(root);
            for (unsigned channel = 0; channel < 2; ++channel)
            {
                allApplied &= manager.drivers[channel].value == revision * 10 + channel;
                allApplied &= manager.drivers[channel].applications == revision;
            }
            allApplied &= manager.EffectEngineIsConfiguredToRun[1] == !(revision & 1);
        }

        releasePoll.set_value();
        mainLoop.join();
        require(allApplied && manager.fallbackConfigs == 0,
                "Repeated input reloads failed to apply the latest input options");
        require(ftp.concurrentLifecycleCalls == 0,
                "Input configuration reload restarted FTP during its main-loop poll");
        require(ftp.lifecycleCalls == 0,
                "Input configuration reload unnecessarily changed the FTP lifecycle");
        std::cout << "Input reload regression passed (" << checks << " checks, 32 reloads).\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
