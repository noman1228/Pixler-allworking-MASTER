#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "output/TestModePatterns.hpp"

class String : public std::string
{
public:
    using std::string::string;
    String(const std::string& value) : std::string(value) {}
    String(const char* value = "") : std::string(value ? value : "") {}
    explicit String(int value) : std::string(std::to_string(value)) {}
    bool isEmpty() const { return empty(); }
    size_t write(uint8_t value) { push_back(char(value)); return 1; }
    size_t write(const uint8_t* data, size_t count) { append(reinterpret_cast<const char*>(data), count); return count; }
};
#include <ArduinoJson.h>
using std::min;
using byte = uint8_t;
#define F(value) value
#define DEBUG_START
#define DEBUG_END
#define DEBUG_V(...)
#define OUTPUT_DATA_GUARD std::lock_guard<std::recursive_mutex> dataGuard(OutputDataMutex)
#define INPUT_STATE_GUARD std::lock_guard<std::recursive_mutex> stateGuard(InputStateMutex)
static const String emptyString;
static const String MN_22 = "Output buffer capacity: ";
static void logcon(const String&) {}
static std::atomic<uint32_t> testClock{0};
static bool rebooting = false;
uint32_t millis() { return testClock.load(); }
bool RebootInProgress() { return rebooting; }

struct c_InputCommon
{
    bool active = true;
    void SetOperationalState(bool value) { active = value; }
};
struct c_InputMgr
{
    struct Driver { c_InputCommon* InputDriver = nullptr; bool DriverInUse = false; };
    Driver InputChannelDrivers[2];
    bool PauseProcessing = false;
    bool TestModePaused = false;
    bool FileExtractionPaused = false;
    uint32_t InputDataBufferSize = 0;
    std::recursive_mutex InputStateMutex;
    void SetOperationalState(bool ActiveFlag);
    void SetTestModePaused(bool Paused);
    void ApplyOperationalState();
    void SetBufferInfo(uint32_t size) { InputDataBufferSize = size; }
} InputMgr;

struct c_OutputCommon
{
    byte* pOutputBuffer = nullptr;
    uint32_t OutputBufferSize = 0;
    uint32_t requestedBytes = 0;
    bool paused = false;
    virtual ~c_OutputCommon() = default;
    virtual uint32_t GetTestPixelCount() { return 0; }
    virtual void WriteTestPixel(uint32_t, uint8_t, uint8_t, uint8_t) {}
    virtual void SetTestModePaused(bool, bool = true) {}
    virtual void GetDriverName(String& name) { name = "Fixture"; }
    virtual uint32_t GetNumOutputBufferBytesNeeded() { return requestedBytes; }
    virtual uint32_t GetNumOutputBufferChannelsServiced() { return requestedBytes; }
    virtual void SetOutputBufferSize(uint32_t size) { OutputBufferSize = size; }
    void SetOutputBufferAddress(byte* address) { pOutputBuffer = address; }
    void PauseOutput(bool value) { paused = value; }
    void WriteChannelData(uint32_t start, uint32_t count, byte* data)
    {
        assert(start + count <= OutputBufferSize);
        std::copy(data, data + count, pOutputBuffer + start);
    }
};
struct c_OutputPixel : c_OutputCommon
{
    uint32_t pixel_count = 10;
    uint32_t NumIntensityBytesPerPixel = 3;
    uint32_t PixelGroupSize = 1;
    uint32_t AdjustedBrightness = 256;
    bool MonsterPermanentRgb = false;
    uint8_t gamma_table[256]{};
    struct { uint8_t Array[4] = {0, 1, 2, 3}; } ColorOffsets;
    float IntensityBitTimeInUs = 1;
    uint32_t BlockSize = 1;
    float BlockDelayUs = 0;
    void SetFrameDurration(float, uint32_t, float) {}
    uint32_t GetNumOutputBufferBytesNeeded() override { return pixel_count * NumIntensityBytesPerPixel; }
    uint32_t GetNumOutputBufferChannelsServiced() override { return GetNumOutputBufferBytesNeeded() / PixelGroupSize; }
    void SetOutputBufferSize(uint32_t NumChannelsAvailable) override;
    uint32_t GetTestPixelCount() override;
    void WriteTestPixel(uint32_t pixel, uint8_t red, uint8_t green, uint8_t blue) override;
    c_OutputPixel() { for (uint32_t i = 0; i < 256; ++i) gamma_table[i] = uint8_t(i); }
};

struct ObservedPixel : c_OutputPixel
{
    unsigned physicalWrites = 0;
    std::function<void()> afterPhysicalWrite;
    void WriteTestPixel(uint32_t pixel, uint8_t red, uint8_t green, uint8_t blue) override
    {
        c_OutputPixel::WriteTestPixel(pixel, red, green, blue);
        ++physicalWrites;
        if (afterPhysicalWrite) { afterPhysicalWrite(); }
    }
};

struct c_OutputMgr
{
    static constexpr int OutputChannelId_End = 4;
    struct DriverInfo_t
    {
        c_OutputCommon* OutputDriver = nullptr;
        bool OutputDriverInUse = true;
        int DriverId = 0;
        uint32_t OutputChannelStartingOffset = 0;
        uint32_t OutputChannelEndOffset = 0;
        uint32_t OutputChannelSize = 0;
        uint32_t OutputBufferStartingOffset = 0;
        uint32_t OutputBufferDataSize = 0;
        uint32_t OutputBufferEndOffset = 0;
    };
    DriverInfo_t OutputChannelDrivers[OutputChannelId_End];
    std::recursive_mutex OutputDataMutex;
    bool HasBeenInitialized = true;
    bool ConfigInProgress = false;
    bool OutputIsPaused = false;
    uint32_t UsedBufferSize = 66;
    uint64_t guardBefore = UINT64_C(0xaabbccdd12345678);
    byte OutputBuffer[9000]{};
    uint64_t guardAfter = UINT64_C(0x88776655ffeeddcc);
    struct TestModeSettings
    {
        bool active = false;
        TestModePatterns::Pattern pattern = TestModePatterns::Pattern::Red;
        int32_t output = -1;
        uint32_t pixel = 1;
        uint8_t brightness = 64;
        uint32_t interval = 250;
        uint32_t segment = 5;
    } TestMode;
    uint32_t TestStartedAt = 0;
    uint32_t LastTestStep = uint32_t(-1);
    bool InputTestPauseApplied = false;
    std::atomic<bool> ConfigReloadStopRequested{false};
    bool SetTestMode(const JsonObject& request, String& error);
    void GetTestModeStatus(JsonObject& status);
    void ServiceTestMode();
    void RenderTestMode();
    void StopTestMode(bool restoreOutputs = true);
    void WriteChannelData(uint32_t start, uint32_t count, byte* data);
    void ClearBuffer();
    void PauseOutputs(bool value);
    void UpdateDisplayBufferReferences(void);
    byte* GetBufferAddress() { return OutputBuffer; }
    uint32_t GetBufferSize() { return sizeof(OutputBuffer); }
} OutputMgr;

#include "physical_methods.inc"
#include "pixel_allocation_method.inc"
#include "runtime_methods.inc"

constexpr int HTTP_GET = 1, HTTP_POST = 2, HTTP_OPTIONS = 4;
static const char* CN_applicationSLASHjson = "application/json";
struct AsyncWebParameter
{
    String text;
    String value() const { return text; }
};
struct AsyncWebServerResponse
{
    int code;
    String contentType;
    String body;
    std::map<String, String> headers;
    void addHeader(const char* name, const char* value) { headers[name] = value; }
};
struct AsyncWebServerRequest
{
    int verb = HTTP_GET;
    std::map<String, AsyncWebParameter> params;
    AsyncWebServerResponse response{0, "", "", {}};
    int method() const { return verb; }
    bool hasParam(const char* name, bool) const { return params.count(name) != 0; }
    const AsyncWebParameter* getParam(const char* name, bool) const { return &params.at(name); }
    AsyncWebServerResponse* beginResponse(int code, const char* type, const String& body)
    {
        return new AsyncWebServerResponse{code, type, body, {}};
    }
    void send(int code) { response.code = code; }
    void send(AsyncWebServerResponse* reply) { response = *reply; delete reply; }
};
struct
{
    std::function<void(AsyncWebServerRequest*)> handler;
    void on(const char* path, int methods, std::function<void(AsyncWebServerRequest*)> callback)
    {
        assert(String(path) == "/api/testmode");
        assert(methods == (HTTP_GET | HTTP_POST | HTTP_OPTIONS));
        handler = callback;
    }
} webServer;
#include "api_route.inc"

static AsyncWebServerResponse http(int verb, std::map<String, AsyncWebParameter> params = {})
{
    AsyncWebServerRequest request;
    request.verb = verb;
    request.params = std::move(params);
    webServer.handler(&request);
    return request.response;
}

static bool command(const char* data, String* failure = nullptr)
{
    JsonDocument document;
    assert(!deserializeJson(document, data));
    JsonObject request = document.as<JsonObject>();
    String error;
    const bool result = OutputMgr.SetTestMode(request, error);
    if (failure) *failure = error;
    else if (!result) std::cerr << error << "\n";
    return result;
}

int main()
{
    auto& OutputDataMutex = OutputMgr.OutputDataMutex;
    c_InputCommon primary, secondary;
    InputMgr.InputChannelDrivers[0] = {&primary, true};
    InputMgr.InputChannelDrivers[1] = {&secondary, true};
    ObservedPixel rgb, rgbw;
    c_OutputCommon other, disabled;
    rgb.pixel_count = 10;
    rgb.OutputBufferSize = 30;
    rgb.pOutputBuffer = OutputMgr.OutputBuffer;
    rgbw.pixel_count = 7;
    rgbw.NumIntensityBytesPerPixel = 4;
    rgbw.OutputBufferSize = 28;
    rgbw.pOutputBuffer = OutputMgr.OutputBuffer + 30;
    other.OutputBufferSize = 8;
    other.requestedBytes = 8;
    other.pOutputBuffer = OutputMgr.OutputBuffer + 58;
    disabled.pOutputBuffer = OutputMgr.OutputBuffer + 66;
    OutputMgr.OutputChannelDrivers[0] = {&rgb, true, 0, 0, 30, 30};
    OutputMgr.OutputChannelDrivers[1] = {&rgbw, true, 1, 30, 58, 28};
    OutputMgr.OutputChannelDrivers[2] = {&other, true, 2, 58, 66, 8};
    OutputMgr.OutputChannelDrivers[3] = {&disabled, true, 3, 66, 66, 0};

    String error;
    assert(!command("{}", &error));
    assert(!error.isEmpty());
    assert(command("{\"active\":true,\"pattern\":\"red\",\"output\":-1}"));
    OutputMgr.ServiceTestMode();
    assert(InputMgr.TestModePaused && !primary.active && !secondary.active);
    InputMgr.SetOperationalState(true);
    assert(!primary.active); // An unrelated resume cannot remove test ownership.
    for (uint32_t i = 0; i < 10; ++i) assert(rgb.pOutputBuffer[i * 3] == 64);
    for (uint32_t i = 0; i < 7; ++i) assert(rgbw.pOutputBuffer[i * 4] == 64);
    for (uint32_t i = 58; i < 66; ++i) assert(OutputMgr.OutputBuffer[i] == 0);

    // Solid colors and the manual count marker must remain untouched across
    // timer intervals. RMT reads asynchronously without the data mutex, so an
    // unnecessary clear-and-refill can appear as a black flash on the wire.
    for (const char* pattern : {"red", "green", "blue", "white", "count"})
    {
        for (unsigned brightness : {1U, 64U, 255U})
        {
            for (unsigned interval : {50U, 5000U})
            {
                testClock = 100;
                const std::string request = std::string("{\"active\":true,\"pattern\":\"") + pattern +
                    "\",\"output\":0,\"pixel\":3,\"segment\":2,\"brightness\":" +
                    std::to_string(brightness) + ",\"interval\":" + std::to_string(interval) + "}";
                assert(command(request.c_str()));
                const std::vector<byte> initial(std::begin(OutputMgr.OutputBuffer), std::end(OutputMgr.OutputBuffer));
                const unsigned writes = rgb.physicalWrites + rgbw.physicalWrites;
                const uint32_t startedAt = OutputMgr.TestStartedAt;
                for (unsigned tick = 1; tick <= 12; ++tick)
                {
                    testClock = 100 + tick * interval;
                    { OUTPUT_DATA_GUARD; OutputMgr.RenderTestMode(); }
                    assert(rgb.physicalWrites + rgbw.physicalWrites == writes);
                    assert(std::equal(initial.begin(), initial.end(), std::begin(OutputMgr.OutputBuffer)));
                }
                // Repeated Apply with identical options must preserve the
                // buffer and original start time, including partial requests.
                assert(command(request.c_str()));
                assert(command("{\"active\":true}"));
                assert(rgb.physicalWrites + rgbw.physicalWrites == writes);
                assert(OutputMgr.TestStartedAt == startedAt);
                assert(std::equal(initial.begin(), initial.end(), std::begin(OutputMgr.OutputBuffer)));
            }
        }
    }

    // At an asynchronous read boundary after every pixel write, an RGB cycle
    // may show old or new colors but must never expose a blanket zeroed frame.
    testClock = 0;
    assert(command("{\"active\":true,\"pattern\":\"rgb\",\"output\":0,\"brightness\":64,\"interval\":50}"));
    unsigned backgroundReads = 0;
    rgb.afterPhysicalWrite = [&] {
        ++backgroundReads;
        for (unsigned pixel = 0; pixel < rgb.pixel_count; ++pixel)
        {
            const byte* data = rgb.pOutputBuffer + pixel * 3;
            assert(data[0] || data[1] || data[2]);
        }
    };
    const unsigned beforeAnimation = rgb.physicalWrites;
    for (unsigned tick = 1; tick <= 6; ++tick)
    {
        testClock = tick * 50;
        { OUTPUT_DATA_GUARD; OutputMgr.RenderTestMode(); }
        assert(rgb.physicalWrites == beforeAnimation + tick * rgb.pixel_count);
    }
    assert(backgroundReads == 6 * rgb.pixel_count);
    rgb.afterPhysicalWrite = {};
    const unsigned animationWrites = rgb.physicalWrites;
    const uint32_t animationStartedAt = OutputMgr.TestStartedAt;
    assert(command("{\"active\":true}"));
    assert(rgb.physicalWrites == animationWrites && OutputMgr.TestStartedAt == animationStartedAt);
    testClock = 350;
    { OUTPUT_DATA_GUARD; OutputMgr.RenderTestMode(); }
    assert(rgb.physicalWrites == animationWrites + rgb.pixel_count);
    assert(rgb.pOutputBuffer[0] == 0 && rgb.pOutputBuffer[1] == 64 && rgb.pOutputBuffer[2] == 0);

    testClock = 0;
    assert(command("{\"active\":true,\"pattern\":\"red\",\"output\":-1,\"pixel\":1,\"segment\":5,\"brightness\":64,\"interval\":250}"));

    // Rejecting a bad command leaves the existing active test intact.
    for (const char* invalid : {
        "{\"active\":true,\"pattern\":\"bogus\"}",
        "{\"active\":true,\"brightness\":0}",
        "{\"active\":true,\"brightness\":256}",
        "{\"active\":true,\"interval\":49}",
        "{\"active\":true,\"interval\":5001}",
        "{\"active\":true,\"segment\":0}",
        "{\"active\":true,\"segment\":1001}",
        "{\"active\":true,\"output\":2}",
        "{\"active\":true,\"output\":4}",
        "{\"active\":true,\"pattern\":\"pixel\",\"output\":-1}",
        "{\"active\":true,\"pattern\":\"pixel\",\"output\":1,\"pixel\":8}",
        "{\"active\":true,\"pattern\":\"count\",\"output\":-1}",
        "{\"active\":true,\"pattern\":\"count\",\"output\":1,\"pixel\":8}",
        "{\"active\":true,\"pattern\":\"count\",\"output\":1,\"segment\":6}",
        "{\"active\":true,\"pixel\":1.5}"})
    {
        assert(!command(invalid, &error));
        assert(OutputMgr.TestMode.active);
        assert(OutputMgr.TestMode.pattern == TestModePatterns::Pattern::Red);
    }

    // A manual white counting marker addresses only the selected physical
    // output, including RGBW's dedicated white channel. It never blinks or
    // wraps; a fresh slider command moves it immediately in the same step.
    assert(command("{\"active\":true,\"pattern\":\"count\",\"output\":1,\"pixel\":7,\"segment\":5,\"brightness\":73}"));
    for (uint32_t pixel = 0; pixel < 7; ++pixel)
    {
        const uint32_t offset = 30 + pixel * 4;
        assert(OutputMgr.OutputBuffer[offset] == 0 && OutputMgr.OutputBuffer[offset + 1] == 0 && OutputMgr.OutputBuffer[offset + 2] == 0);
        assert(OutputMgr.OutputBuffer[offset + 3] == (pixel == 6 ? 73 : 0));
    }
    for (uint32_t i = 0; i < 30; ++i) assert(OutputMgr.OutputBuffer[i] == 0);
    for (uint32_t i = 58; i < 66; ++i) assert(OutputMgr.OutputBuffer[i] == 0);
    testClock = 250;
    { OUTPUT_DATA_GUARD; OutputMgr.RenderTestMode(); }
    assert(OutputMgr.OutputBuffer[41] == 0 && OutputMgr.OutputBuffer[57] == 73);
    assert(command("{\"active\":true,\"pixel\":1,\"segment\":2}"));
    assert(OutputMgr.OutputBuffer[33] == 73 && OutputMgr.OutputBuffer[37] == 73);
    for (uint32_t i = 38; i < 66; ++i) assert(OutputMgr.OutputBuffer[i] == 0);
    JsonDocument countStatusDoc;
    JsonObject countStatus = countStatusDoc.to<JsonObject>();
    OutputMgr.GetTestModeStatus(countStatus);
    assert(String(countStatus["pattern"].as<const char*>()) == "count");
    assert(countStatus["pixel"].as<uint32_t>() == 1 && countStatus["segment"].as<uint32_t>() == 2);
    testClock = 0;

    assert(command("{\"active\":true,\"pattern\":\"pixel\",\"output\":1,\"pixel\":7,\"brightness\":90,\"interval\":50}"));
    for (uint32_t i = 0; i < 57; ++i) assert(OutputMgr.OutputBuffer[i] == 0);
    assert(OutputMgr.OutputBuffer[57] == 90);
    byte normal[6] = {1, 2, 3, 4, 5, 6};
    OutputMgr.WriteChannelData(0, 6, normal);
    OutputMgr.ClearBuffer();
    assert(OutputMgr.OutputBuffer[57] == 90);

    std::atomic<bool> finished{false};
    std::thread normalTraffic([&] {
        while (!finished.load()) { OutputMgr.WriteChannelData(0, 6, normal); OutputMgr.ClearBuffer(); }
    });
    for (uint32_t step = 0; step < 30; ++step)
    {
        testClock = step * 50;
        OUTPUT_DATA_GUARD;
        OutputMgr.RenderTestMode();
        for (uint32_t i = 0; i < 57; ++i) assert(OutputMgr.OutputBuffer[i] == 0);
        assert(OutputMgr.OutputBuffer[57] == (step % 2 ? 0 : 90));
    }
    finished = true;
    normalTraffic.join();

    // Millisecond rollover preserves step timing through unsigned subtraction.
    testClock = UINT32_MAX - 24;
    assert(command("{\"active\":true,\"pattern\":\"rgb\",\"output\":0,\"interval\":50}"));
    testClock = 25;
    { OUTPUT_DATA_GUARD; OutputMgr.RenderTestMode(); }
    assert(rgb.pOutputBuffer[0] == 0 && rgb.pOutputBuffer[1] == 90 && rgb.pOutputBuffer[2] == 0);
    assert(command("{\"active\":false}"));
    OutputMgr.ServiceTestMode();
    assert(!InputMgr.TestModePaused && primary.active && secondary.active);
    for (byte value : OutputMgr.OutputBuffer) assert(value == 0);
    OutputMgr.WriteChannelData(0, 6, normal);
    assert(rgb.pOutputBuffer[0] == 1 && rgb.pOutputBuffer[5] == 6);

    // Preserve pre-existing pause reasons across a test start and stop.
    InputMgr.SetOperationalState(false);
    assert(command("{\"active\":true}"));
    OutputMgr.ServiceTestMode();
    assert(command("{\"active\":false}"));
    OutputMgr.ServiceTestMode();
    assert(InputMgr.PauseProcessing && !primary.active);
    InputMgr.SetOperationalState(true);
    assert(command("{\"active\":true}"));
    OutputMgr.ServiceTestMode();
    rebooting = true;
    OutputMgr.ServiceTestMode();
    assert(!OutputMgr.TestMode.active);

    rebooting = false;

    JsonDocument statusDoc;
    JsonObject status = statusDoc.to<JsonObject>();
    OutputMgr.GetTestModeStatus(status);
    assert(status["outputs"].as<JsonArray>().size() == 2);
    assert(status["totalPixels"].as<uint32_t>() == 17);
    assert(status["outputs"][1]["pixels"].as<uint32_t>() == 7);

    // Run the real HTTP callback against lightweight request/response stubs.
    InstallTestApi();
    assert(http(HTTP_OPTIONS).code == 200);
    auto response = http(HTTP_GET);
    assert(response.code == 200 && response.headers["Cache-Control"] == "no-store");
    assert(!deserializeJson(statusDoc, response.body));
    assert(statusDoc["totalPixels"].as<uint32_t>() == 17);
    assert(http(HTTP_POST).code == 400);
    assert(http(HTTP_POST, {{"active", {"yes"}}}).code == 400);
    for (const char* invalid : {"1.5", "", "-", "+1", " 1", "1 ", "2147483648", "-2147483649", "999999999999"})
    {
        response = http(HTTP_POST, {{"active", {"true"}}, {"pixel", {invalid}}});
        assert(response.code == 400);
        assert(!deserializeJson(statusDoc, response.body));
        assert(statusDoc["error"].is<const char*>());
    }
    assert(http(HTTP_POST, {{"active", {"true"}}, {"pattern", {"pixel"}}, {"output", {"1"}}, {"pixel", {"8"}}}).code == 409);
    assert(!OutputMgr.TestMode.active);
    assert(http(HTTP_POST, {{"active", {"1"}}, {"pattern", {"pixel"}}, {"output", {"1"}}, {"pixel", {"7"}}}).code == 200);
    assert(OutputMgr.TestMode.active && OutputMgr.TestMode.pixel == 7);
    assert(http(HTTP_POST, {{"active", {"false"}}, {"pixel", {"invalid"}}, {"brightness", {"invalid"}}}).code == 200);
    OutputMgr.ServiceTestMode();
    assert(!OutputMgr.TestMode.active);

    response = http(HTTP_POST, {{"active", {"true"}}, {"pattern", {"count"}}, {"output", {"0"}}, {"pixel", {"10"}}, {"segment", {"5"}}});
    assert(response.code == 200);
    assert(!deserializeJson(statusDoc, response.body));
    assert(String(statusDoc["pattern"].as<const char*>()) == "count");
    assert(statusDoc["pixel"].as<uint32_t>() == 10 && statusDoc["segment"].as<uint32_t>() == 5);
    for (uint32_t i = 0; i < 27; ++i) assert(OutputMgr.OutputBuffer[i] == 0);
    for (uint32_t i = 27; i < 30; ++i) assert(OutputMgr.OutputBuffer[i] == 90);
    assert(http(HTTP_POST, {{"active", {"true"}}, {"segment", {"6"}}}).code == 409);
    assert(OutputMgr.TestMode.active && OutputMgr.TestMode.segment == 5);
    assert(http(HTTP_POST, {{"active", {"false"}}}).code == 200);
    OutputMgr.ServiceTestMode();

    // Reallocation must remove exhausted ports' previous storage before Test
    // Mode reads their physical pixel counts. These transitions reproduce a
    // formerly valid second port becoming one-past-end when the first grows.
    rgb.pixel_count = 1500;
    rgbw.pixel_count = 500;
    rgbw.NumIntensityBytesPerPixel = 3;
    OutputMgr.UpdateDisplayBufferReferences();
    assert(OutputMgr.UsedBufferSize == 6008);
    assert(rgb.GetTestPixelCount() == 1500 && rgbw.GetTestPixelCount() == 500);
    assert(rgb.pOutputBuffer == OutputMgr.OutputBuffer);
    assert(rgbw.pOutputBuffer == OutputMgr.OutputBuffer + 4500);
    assert(other.pOutputBuffer == OutputMgr.OutputBuffer + 6000);
    rgb.pixel_count = 3000;
    OutputMgr.UpdateDisplayBufferReferences();
    assert(OutputMgr.UsedBufferSize == 9000 && InputMgr.InputDataBufferSize == 9000);
    assert(rgb.GetTestPixelCount() == 3000);
    assert(rgb.OutputBufferSize == 9000);
    for (size_t id = 1; id < 4; ++id)
    {
        const auto& allocation = OutputMgr.OutputChannelDrivers[id];
        assert(allocation.OutputBufferDataSize == 0);
        assert(allocation.OutputChannelSize == 0);
        assert(allocation.OutputDriver->OutputBufferSize == 0);
        assert(allocation.OutputDriver->GetTestPixelCount() == 0);
    }
    assert(command("{\"active\":true,\"pattern\":\"red\",\"output\":-1}"));
    assert(OutputMgr.OutputBuffer[8997] == 90);
    assert(OutputMgr.guardBefore == UINT64_C(0xaabbccdd12345678));
    assert(OutputMgr.guardAfter == UINT64_C(0x88776655ffeeddcc));
    assert(command("{\"active\":false}"));

    // A partially fitting layout keeps its valid prefix, clears every rejected
    // port, and can subsequently recover both ports after configuration shrinks.
    rgb.pixel_count = 2800;
    OutputMgr.UpdateDisplayBufferReferences();
    assert(OutputMgr.UsedBufferSize == 8400);
    assert(rgb.GetTestPixelCount() == 2800);
    for (size_t id = 1; id < 4; ++id)
    {
        assert(OutputMgr.OutputChannelDrivers[id].OutputBufferDataSize == 0);
        assert(OutputMgr.OutputChannelDrivers[id].OutputChannelSize == 0);
        assert(OutputMgr.OutputChannelDrivers[id].OutputDriver->OutputBufferSize == 0);
    }
    rgb.pixel_count = 1500;
    OutputMgr.UpdateDisplayBufferReferences();
    assert(OutputMgr.UsedBufferSize == 6008);
    assert(rgb.GetTestPixelCount() == 1500 && rgbw.GetTestPixelCount() == 500);
    assert(rgbw.pOutputBuffer == OutputMgr.OutputBuffer + 4500);
    assert(other.pOutputBuffer == OutputMgr.OutputBuffer + 6000);
    assert(OutputMgr.OutputChannelDrivers[1].OutputChannelStartingOffset == 4500);
    assert(OutputMgr.OutputChannelDrivers[1].OutputChannelEndOffset == 6000);

    // Even the first output can exceed capacity; no prior mapping may survive.
    rgb.pixel_count = 3001;
    OutputMgr.UpdateDisplayBufferReferences();
    assert(OutputMgr.UsedBufferSize == 0 && InputMgr.InputDataBufferSize == 0);
    for (const auto& allocation : OutputMgr.OutputChannelDrivers)
    {
        assert(allocation.OutputBufferDataSize == 0);
        assert(allocation.OutputChannelSize == 0);
        assert(allocation.OutputDriver->OutputBufferSize == 0);
        assert(allocation.OutputDriver->GetTestPixelCount() == 0);
    }
    assert(!command("{\"active\":true,\"output\":-1}", &error));
    std::cout << "Test Mode runtime ownership, bounds, concurrency, stop/resume regressions passed\n";
    std::cout << "Test Mode HTTP parsing and status regressions passed\n";
    std::cout << "Test Mode output reallocation and exhausted-port regressions passed\n";
}
