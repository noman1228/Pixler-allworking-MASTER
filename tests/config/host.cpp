#include "../unzip/shims/ESPixelStick.h"
#include <set>

static unsigned checks = 0;
static void require(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

inline const char* CN_Configuration_File_colon = "Configuration File: ";
inline const char* CN_w = "w";
inline const char* CN_r = "r";
inline const char* CN_slashsdslash = "/sd/";
inline const char* CN_applicationSLASHjson = "application/json";
enum class SeekMode { SeekSet };
#define ConnrectFilename(name) if (name.indexOf("/") != 0) { name = String("/") + name; }

namespace fs {
struct File {
    String path;
    bool opened = false, append = false;
    size_t position = 0;
    explicit operator bool() const { return opened; }
    size_t size() const;
    bool seek(size_t value, SeekMode);
    size_t write(const uint8_t* data, size_t count);
    int read();
    size_t readBytes(char* data, size_t count);
    void close() { opened = false; }
};
}

struct Storage {
    std::map<String, String> files;
    std::set<String> failedOpens;
    size_t writeLimit = SIZE_MAX;
    bool seekFails = false, renameFails = false;
    unsigned removals = 0, renames = 0;
    bool exists(const String& path) const { return files.count(path) != 0; }
    bool remove(const String& path) { ++removals; return files.erase(path) != 0; }
    bool rename(const String& from, const String& to) {
        ++renames;
        const auto found = files.find(from);
        if (renameFails || found == files.end()) return false;
        // LittleFS atomically replaces an existing file; no predelete is needed.
        files[to] = found->second;
        files.erase(found);
        return true;
    }
    fs::File open(const char* name, const char* mode) {
        String path(name);
        if (!path.startsWith("/") || failedOpens.count(path)) return {};
        if (mode[0] == 'w') files[path].clear();
        else if (mode[0] == 'a') files.try_emplace(path, "");
        else if (!exists(path)) return {};
        return {path, true, mode[0] == 'a', mode[0] == 'a' ? files.at(path).size() : 0};
    }
} LittleFS;

size_t fs::File::size() const { return LittleFS.files.at(path).size(); }
bool fs::File::seek(size_t value, SeekMode) {
    if (LittleFS.seekFails) return false;
    position = value;
    return true;
}
size_t fs::File::write(const uint8_t* data, size_t count) {
    count = std::min(count, LittleFS.writeLimit);
    auto& content = LittleFS.files.at(path);
    if (append) position = content.size();
    static_cast<std::string&>(content).replace(position, count, reinterpret_cast<const char*>(data), count);
    position += count;
    return count;
}
int fs::File::read() {
    const auto& content = LittleFS.files.at(path);
    return position < content.size() ? uint8_t(content[position++]) : -1;
}
size_t fs::File::readBytes(char* data, size_t count) {
    const auto& content = LittleFS.files.at(path);
    count = std::min(count, content.size() - position);
    std::copy_n(content.data() + position, count, data);
    position += count;
    return count;
}

struct c_FileMgr {
    unsigned scans = 0;
    void DeleteFlashFile(String name);
    bool RenameFlashFile(String from, String to);
    bool SaveFlashFile(const String& path, uint32_t index, uint8_t* data, uint32_t len, bool final);
    using DeserializationHandler = std::function<void(JsonDocument&)>;
    bool LoadFlashFile(const String& path, DeserializationHandler handler);
    void BuildFseqList(bool) { ++scans; }
} FileMgr;
#include "flash_methods.inc"

using WebRequestMethodComposite = int;
constexpr int HTTP_GET = 1, HTTP_PUT = 2, HTTP_POST = 4, HTTP_OPTIONS = 8;
struct AsyncWebServerRequest {
    String path = "/conf/input_config.json";
    int verb = HTTP_PUT, response = 0;
    unsigned responses = 0;
    const String& url() const { return path; }
    int method() const { return verb; }
    const int* getResponse() const { return response ? &response : nullptr; }
    void send(int code, const char* = "", const char* = "") { response = code; ++responses; }
};
struct Server {
    std::function<void(AsyncWebServerRequest*)> complete;
    std::function<void(AsyncWebServerRequest*, String, uint32_t, uint8_t*, uint32_t, bool)> upload;
    std::function<void(AsyncWebServerRequest*, uint8_t*, uint32_t, uint32_t, uint32_t)> body;
    template<class C, class U, class B>
    void on(const char*, int, C c, U u, B b) { complete = c; upload = u; body = b; }
} webServer;
static unsigned rebootDelays = 0;
void DelayReboot(unsigned) { ++rebootDelays; }
#ifdef CONFIG_RUNTIME_FIXTURE
#include "live_managers.hpp"
#endif
struct c_WebMgr {
    std::vector<String> reloads;
#ifdef CONFIG_RUNTIME_FIXTURE
    bool RequestReadConfigFile(String& name);
#else
    bool RequestReadConfigFile(String& name) { reloads.push_back(name); return true; }
#endif
    void Install();
} WebMgr;
#ifdef CONFIG_RUNTIME_FIXTURE
#include "request_reload_method.inc"
#endif
#include "config_route.inc"

static const String oldConfig = "{\"input_config\":\"old\"}";
static const String newConfig = "{\"input_config\":\"new and longer\"}";
static void reset() {
    LittleFS = {};
    FileMgr.scans = 0;
    WebMgr.reloads.clear();
    rebootDelays = 0;
    LittleFS.files["/input_config.json"] = oldConfig;
}
static void chunk(AsyncWebServerRequest& request, bool multipart, uint32_t index, uint32_t length) {
    auto* bytes = reinterpret_cast<uint8_t*>(const_cast<char*>(newConfig.data() + index));
    if (multipart) webServer.upload(&request, "input_config.json", index, bytes, length, index + length == newConfig.size());
    else webServer.body(&request, bytes, length, index, uint32_t(newConfig.size()));
}
static void preserved() {
    require(LittleFS.files.at("/input_config.json") == oldConfig, "Previous config was deleted or changed before commit");
    require(FileMgr.scans == 0, "Flash config operation rescanned SD card");
}

int main() {
    try {
        WebMgr.Install();
        for (bool multipart : {false, true}) {
            reset();
            AsyncWebServerRequest success;
            chunk(success, multipart, 0, 9);
            preserved();
            chunk(success, multipart, 9, uint32_t(newConfig.size() - 9));
            preserved();
            webServer.complete(&success);
            require(success.response == 200 && success.responses == 1, "Completed upload did not return one success");
            require(LittleFS.files.at("/input_config.json") == newConfig, "Config replacement lost chunk data");
            require(!LittleFS.exists("/input_config.json.tmp"), "Committed temporary file survived");
            require(WebMgr.reloads == std::vector<String>{"input_config.json"}, "Committed config was not reloaded once");
            require(LittleFS.removals == 0 && FileMgr.scans == 0, "Successful config commit predeleted or rescanned storage");

            for (int fault = 0; fault < 5; ++fault) {
                reset();
                AsyncWebServerRequest request;
                if (fault == 0) LittleFS.failedOpens.insert("/input_config.json.tmp");
                if (fault == 1) LittleFS.writeLimit = 3;
                if (fault == 2) LittleFS.seekFails = true;
                if (fault == 3) LittleFS.renameFails = true;
                if (fault == 4) chunk(request, multipart, 0, 9);
                chunk(request, multipart, fault == 4 ? 12 : 0, fault == 4 ? 4 : uint32_t(newConfig.size()));
                preserved();
                webServer.complete(&request);
                require(request.response >= 400 && request.responses == 1, "Failed upload reported success or replied twice");
                require(WebMgr.reloads.empty(), "Failed upload scheduled a config reload");
                require(!LittleFS.exists("/input_config.json.tmp"), "Failed temporary config survived cleanup");
                preserved();
                chunk(request, multipart, 0, 5);
                webServer.complete(&request);
                require(request.responses == 1 && !LittleFS.exists("/input_config.json.tmp"), "Failed request restarted or committed during later callbacks");
            }
        }

        reset();
        AsyncWebServerRequest missing;
        webServer.complete(&missing);
        require(missing.response == 500 && WebMgr.reloads.empty(), "Missing staged config scheduled reload");
        preserved();

        reset();
        AsyncWebServerRequest unsupported;
        unsupported.path = "/conf/admininfo.json";
        LittleFS.files["/admininfo.json"] = "original";
        LittleFS.files["/admininfo.json.tmp"] = "replacement";
        webServer.complete(&unsupported);
        require(unsupported.response == 404 && WebMgr.reloads.empty(), "Unsupported config was accepted");
        require(LittleFS.files.at("/admininfo.json") == "original", "Unsupported config overwrote an existing file");

        reset();
        require(!FileMgr.RenameFlashFile("missing.tmp", "input_config.json"), "Missing source rename succeeded");
        preserved();
        FileMgr.DeleteFlashFile("input_config.json");
        require(!LittleFS.exists("/input_config.json") && FileMgr.scans == 0, "Explicit config delete did not stay on flash");
        LittleFS.files["/sd/a.fseq"] = "sequence";
        require(FileMgr.RenameFlashFile("sd/a.fseq", "sd/b.fseq"), "Simulated SD rename failed");
        FileMgr.DeleteFlashFile("sd/b.fseq");
#ifdef SIMULATE_SD
        require(FileMgr.scans == 2, "Simulated SD mutations did not refresh sequence list");
#else
        require(FileMgr.scans == 0, "Native flash mutation rescanned SD");
#endif
        std::cout << "Config upload regression passed: " << checks << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
