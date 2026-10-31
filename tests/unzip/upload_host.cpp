#include "shims/ESPixelStick.h"
#include <set>
#include <atomic>
#include <new>

#define SUPPORT_UNZIP
#define SD_BLOCK_SIZE 512
#define MAX_SD_BUFFER_SIZE 7168
inline const char* CN_applicationSLASHjson = "application/json";
inline const char* CN_filename = "filename";
inline String int64String(uint64_t value) { return String(value); }
inline uint32_t now() { return millis() / 1000; }

static unsigned checks = 0;
static void require(bool value, const char* reason)
{
    ++checks;
    if (!value) throw std::runtime_error(reason);
}

struct Storage
{
    std::map<String, std::vector<byte>> files;
    std::set<std::pair<String, String>> failedRenames;
    size_t writeLimit = SIZE_MAX;
    bool syncFails = false;
    uint32_t totalClusters = 1000, freeClusters = 750;
    bool exists(const char* name) const { return files.count(name) != 0; }
    bool remove(const String& name) { return files.erase(name) != 0; }
    bool rename(const char* from, const char* to)
    {
        const auto source = files.find(from);
        if (source == files.end() || files.count(to) || failedRenames.count({from, to})) return false;
        files[to] = source->second;
        files.erase(source);
        return true;
    }
    void chdir() {}
    uint32_t bytesPerCluster() const { return 4096; }
    uint32_t clusterCount() const { return totalClusters; }
    uint32_t freeClusterCount() const { return freeClusters; }
} sd;
#define ESP_SD sd

struct FakeFile
{
    String name;
    bool failed = false;
    size_t write(const byte* data, size_t count)
    {
        const size_t accepted = min(count, sd.writeLimit);
        if (accepted) sd.files[name].insert(sd.files[name].end(), data, data + accepted);
        failed = accepted != count;
        return accepted;
    }
    bool sync() { return !sd.syncFails; }
    bool getWriteError() const { return failed; }
};

struct Outputs
{
    byte storage[8194]{};
    uint32_t bufferSize = 1024;
    bool paused = false;
    uint32_t GetBufferSize() const { return bufferSize; }
    byte* GetBufferAddress() { return storage + 1; }
    void ClearBuffer() {}
    void PauseOutputs(bool value) { paused = value; }
} OutputMgr;
struct Inputs { bool active = true; void SetOperationalState(bool value) { active = value; } } InputMgr;
struct Unzip
{
    unsigned scans = 0;
    unsigned uploadReservations = 0;
    std::set<String> inUse;
    void Run() { ++scans; }
    bool IsFileInUse(const String& name) const { return inUse.count(name) != 0; }
    bool TryBeginUpload(const String& name, std::atomic<bool>& active)
    {
        ++uploadReservations;
        if (IsFileInUse(name)) return false;
        active.store(true);
        return true;
    }
} gUnzipFiles;

struct Parameter { String text; const String& value() const { return text; } };
struct AsyncWebServerRequest
{
    int response = 0;
    String body;
    String urlText;
    std::map<String, Parameter> params;
    std::function<void()> disconnect;
    const int* getResponse() const { return response ? &response : nullptr; }
    void send(int status, const char* = "", const char* = "") { response = status; }
    void send(int status, const char*, const String& value) { response = status; body = value; }
    void onDisconnect(std::function<void()> value) { disconnect = value; }
    bool hasParam(const String& name) const { return params.count(name) != 0; }
    Parameter* getParam(const String& name) { return &params.at(name); }
    const String& url() const { return urlText; }
};
inline void printReq(AsyncWebServerRequest*, bool) {}

class c_FPPDiscovery
{
public:
    std::atomic<bool> inFileUpload {false};
    bool writeFailed = false;
    AsyncWebServerRequest* uploadRequest = nullptr;
    String UploadFileName;
    String ulrPath = "path";
    unsigned stops = 0;
    unsigned metadataReads = 0;
    uint32_t LastFppMasterMessageRcvTime = 0;
    struct { void Take() {} void Give() {} } FPPSemaphore;
    void StopPlaying() { ++stops; }
    bool IsUploadInProgress() const { return inFileUpload; }
    void ProcessFile(AsyncWebServerRequest*, String, uint32_t, uint8_t*, uint32_t, bool, uint32_t = 0);
    void ProcessBody(AsyncWebServerRequest*, uint8_t*, uint32_t, uint32_t, uint32_t);
    void FinishFileUpload(AsyncWebServerRequest*);
    void ProcessPOST(AsyncWebServerRequest*);
    void ProcessGET(AsyncWebServerRequest*);
    bool AllowedToPlayRemoteFile() const { return true; }
    void GetStatusJSON(JsonObject&, bool) {}
    void GetSysInfoJSON(JsonObject&) {}
    void BuildFseqResponse(String, uint32_t, String& value) { ++metadataReads; value = "fseq metadata"; }
} FPPDiscovery;

class c_FileMgr
{
public:
    using FileId = uint32_t;
    static constexpr FileId INVALID_FILE_HANDLE = 0;
    enum class FileMode { FileRead, FileWrite, FileAppend };
    struct SdInfo { uint64_t MaxSize = 0, Available = 0, Used = 0; };
    struct Entry
    {
        FakeFile fsFile;
        String Filename;
        bool IsOpen = false;
        struct { byte* DataBuffer = nullptr; uint64_t size = 0, offset = 0; } buffer;
    } FileList[2];
    bool SdCardInstalled = true, fsUploadInProgress = false;
    FileId fsUploadFileHandle = INVALID_FILE_HANDLE;
    String fsUploadFileName;
    uint32_t expectedIndex = 0, fsUploadStartTime = 0;
    bool isOpen = false;
    unsigned rebuilds = 0;
    void LockSd() {}
    void UnLockSd() {}
    bool SdCardIsInstalled() const { return SdCardInstalled; }
    int FileListFindSdFileHandle(FileId handle) const { return isOpen && handle == 1 ? 0 : -1; }
    bool OpenSdFile(const String& name, FileMode mode, FileId& handle, int)
    {
        if (!SdCardInstalled) return false;
        if (mode == FileMode::FileRead) {
            if (!sd.files.count(name)) return false;
        } else sd.files[name] = {};
        FileList[0].fsFile = FakeFile{name};
        FileList[0].Filename = name;
        FileList[0].IsOpen = true;
        handle = 1;
        isOpen = true;
        return true;
    }
    void CloseSdFile(FileId& handle) { handle = 0; isOpen = false; FileList[0].IsOpen = false; FileList[0].buffer = {}; }
    void DeleteSdFile(const String& name) { sd.files.erase(name); }
    void BuildFseqList(bool) { ++rebuilds; }
    uint64_t GetSdFileSize(const String& name) const { auto file = sd.files.find(name); return file == sd.files.end() ? 0 : file->second.size(); }
    uint64_t GetSdFileSize(const FileId& handle) const { return FileListFindSdFileHandle(handle) < 0 ? 0 : GetSdFileSize(FileList[0].Filename); }
    uint64_t WriteSdFile(const FileId& handle, byte* data, uint64_t count)
    {
        if (FileListFindSdFileHandle(handle) < 0) return 0;
        size_t result = FileList[0].fsFile.write(data, size_t(count));
        return FileList[0].fsFile.sync() ? result : 0;
    }
    uint64_t WriteSdFileBuf(const FileId&, byte*, uint64_t);
    bool FlushSdFileBuffer(const FileId&);
    bool handleFileUpload(const String&, size_t, uint8_t*, size_t, bool, uint32_t);
    void handleFileUploadNewFile(const String&);
    void AbortSdFileUpload();
    bool IsUploadInProgress() const;
    bool IsCompressed(String);
    bool ReplaceSdFile(const String&, const String&);
    void GetSdInfo(SdInfo&);
} FileMgr;

#include "upload_methods.inc"
#include "fpp_upload_methods.inc"

static void reset()
{
    sd = Storage{};
    FileMgr = c_FileMgr{};
    FPPDiscovery.~c_FPPDiscovery();
    new (&FPPDiscovery) c_FPPDiscovery();
    OutputMgr = Outputs{};
    InputMgr = Inputs{};
    gUnzipFiles = Unzip{};
    testClock = 100;
    OutputMgr.storage[0] = OutputMgr.storage[8193] = 0xa5;
}

static std::vector<byte> payload(size_t count)
{
    std::vector<byte> result(count);
    for (size_t n = 0; n < count; ++n) result[n] = byte(n * 17 + 3);
    return result;
}

static void bufferingAndFinalization()
{
    for (const uint32_t size : {0U, 1U, 511U, 512U, 1024U, 4096U, 8192U}) {
        reset();
        OutputMgr.bufferSize = size;
        auto bytes = payload(24'157);
        require(FileMgr.handleFileUpload("seq.xlz", 0, bytes.data(), 101, false, 25'000), "first chunk failed");
        require(FileMgr.IsUploadInProgress(), "active upload was not reported");
        require(gUnzipFiles.scans == 0, "archive queued before closing upload");
        require(FileMgr.handleFileUpload("seq.xlz", 101, bytes.data() + 101, bytes.size() - 101, true, 25'000), "oversized upload chunk or multipart length rejected");
        require(sd.files.at("seq.xlz") == bytes, "buffering corrupted payload");
        require(gUnzipFiles.scans == 1 && !FileMgr.IsUploadInProgress(), "completed archive was not queued exactly once");
        require(OutputMgr.storage[0] == 0xa5 && OutputMgr.storage[8193] == 0xa5, "upload overflowed output buffer");
    }
    reset();
    auto bytes = payload(14);
    require(FileMgr.handleFileUpload("last.xlz", 0, bytes.data(), bytes.size(), false, 999), "buffered chunk failed");
    sd.writeLimit = 2;
    require(!FileMgr.handleFileUpload("last.xlz", bytes.size(), nullptr, 0, true, 999), "short final flush was accepted");
    require(!sd.files.count("last.xlz") && gUnzipFiles.scans == 0, "failed final flush left/queued archive");

    reset();
    require(FileMgr.handleFileUpload("sync.zip", 0, bytes.data(), bytes.size(), false, 999), "sync test first chunk failed");
    sd.syncFails = true;
    require(!FileMgr.handleFileUpload("sync.zip", bytes.size(), nullptr, 0, true, 999), "failed final sync was accepted");
    require(!sd.files.count("sync.zip") && !FileMgr.IsUploadInProgress(), "sync failure did not clean upload state");

    reset();
    require(FileMgr.handleFileUpload("bad-index.xlz", 0, bytes.data(), 5, false, 99), "index test first chunk failed");
    require(!FileMgr.handleFileUpload("bad-index.xlz", 6, bytes.data(), 5, true, 99), "noncontiguous payload accepted");
    require(!sd.files.count("bad-index.xlz"), "bad-index partial archive survived");

    reset();
    require(FileMgr.handleFileUpload("bad-size.xlz", 0, bytes.data(), 5, false, 99), "size test first chunk failed");
    sd.files["bad-size.xlz"].push_back(1);
    require(!FileMgr.handleFileUpload("bad-size.xlz", 5, bytes.data() + 5, 9, true, 99), "stored-size mismatch accepted");
    require(!sd.files.count("bad-size.xlz"), "size-mismatch archive survived");

    for (const auto& name : {"UPPER.XLZ", "upper.ZIP", "a.fseq", "not.zip.tmp"}) {
        reset();
        require(FileMgr.handleFileUpload(name, 0, bytes.data(), bytes.size(), true, 999), "suffix test upload failed");
        require(gUnzipFiles.scans == (String(name).endsWith("XLZ") || String(name).endsWith("ZIP") ? 1U : 0U), "archive suffix matching is wrong");
    }
}

static void replacementFaults()
{
    const std::vector<byte> good{1, 2, 3}, newer{4, 5, 6, 7};
    reset(); sd.files[".unzip.tmp"] = newer; sd.files["seq.fseq"] = good;
    require(FileMgr.ReplaceSdFile(".unzip.tmp", "seq.fseq"), "valid replacement failed");
    require(sd.files.at("seq.fseq") == newer && sd.files.size() == 1, "replacement left wrong bytes or backup");

    reset(); sd.files["seq.fseq"] = good;
    require(!FileMgr.ReplaceSdFile("missing", "seq.fseq"), "missing staged file accepted");
    require(sd.files.at("seq.fseq") == good, "missing stage destroyed old file");
    require(FileMgr.ReplaceSdFile("seq.fseq", "seq.fseq"), "same-name replacement not a no-op");

    reset(); sd.files[".unzip.tmp"] = newer; sd.files["seq.fseq"] = good;
    sd.failedRenames.insert({"seq.fseq", "seq.fseq.unzip.bak"});
    require(!FileMgr.ReplaceSdFile(".unzip.tmp", "seq.fseq"), "backup rename fault ignored");
    require(sd.files.at("seq.fseq") == good, "backup rename fault lost good target");

    reset(); sd.files[".unzip.tmp"] = newer; sd.files["seq.fseq"] = good;
    sd.failedRenames.insert({".unzip.tmp", "seq.fseq"});
    require(!FileMgr.ReplaceSdFile(".unzip.tmp", "seq.fseq"), "commit rename fault ignored");
    require(sd.files.at("seq.fseq") == good, "commit fault did not restore previous sequence");
    sd.failedRenames.insert({"seq.fseq.unzip.bak", "seq.fseq"});
    require(!FileMgr.ReplaceSdFile(".unzip.tmp", "seq.fseq"), "rollback failure incorrectly succeeded");
    require(sd.files.at("seq.fseq.unzip.bak") == good, "failed rollback destroyed recovery copy");
    sd.failedRenames.clear();
    require(FileMgr.ReplaceSdFile(".unzip.tmp", "seq.fseq"), "recoverable failed replacement could not retry");
    require(sd.files.at("seq.fseq") == newer && sd.files.size() == 1, "retry failed to preserve final target");

    reset(); sd.files[".unzip.tmp"] = newer; sd.files["seq.fseq"] = good; sd.files["seq.fseq.unzip.bak"] = {9};
    require(!FileMgr.ReplaceSdFile(".unzip.tmp", "seq.fseq"), "existing recovery copy overwritten");
    require(sd.files.at("seq.fseq") == good && sd.files.at("seq.fseq.unzip.bak") == std::vector<byte>{9}, "conflicting recovery data changed");

    for (const auto& openedName : {"seq.fseq", "/SEQ.FSEQ", " / seq.fseq. ", "/dir/../seq.fseq", "//./seq.fseq", "seq.fseq.UNZIP.BAK", "/ seq.fseq.unzip.bak. "}) {
        reset(); sd.files[".unzip.tmp"] = newer; sd.files["seq.fseq"] = good;
        FileMgr.FileList[1].Filename = openedName;
        FileMgr.FileList[1].IsOpen = true;
        require(!FileMgr.ReplaceSdFile(".unzip.tmp", "seq.fseq"), "open target or backup alias was replaced");
        require(sd.files.at("seq.fseq") == good && sd.files.at(".unzip.tmp") == newer && sd.files.size() == 2, "refused open-file replacement changed storage");
        FileMgr.FileList[1].IsOpen = false;
        require(FileMgr.ReplaceSdFile(".unzip.tmp", "seq.fseq") && sd.files.at("seq.fseq") == newer, "closed metadata reader prevented replacement");
    }
}

static void fppRequests()
{
    reset();
    auto bytes = payload(1800);
    AsyncWebServerRequest request;
    String longName = String(std::string(170, 's')) + ".xlz";
    FPPDiscovery.ProcessFile(&request, longName, 0, bytes.data(), 100, false);
    require(FileMgr.IsUploadInProgress() && InputMgr.active == false && OutputMgr.paused && gUnzipFiles.uploadReservations == 1, "FPP transfer did not reserve extraction or suspend playback");
    require(FPPDiscovery.UploadFileName == longName, "FPP filename was truncated");
    AsyncWebServerRequest other;
    FPPDiscovery.ProcessFile(&other, "other.xlz", 0, bytes.data(), 100, true);
    require(other.response == 409 && FileMgr.IsUploadInProgress(), "concurrent request corrupted upload owner");
    FPPDiscovery.ProcessFile(&request, longName, 100, bytes.data() + 100, 1700, true);
    require(sd.files.at(longName) == bytes && gUnzipFiles.scans == 1, "long-name FPP archive upload failed");
    require(FileMgr.IsUploadInProgress(), "extraction was released before upload response was built");
    FPPDiscovery.FinishFileUpload(&request);
    require(!FileMgr.IsUploadInProgress() && InputMgr.active && !OutputMgr.paused, "FPP completion did not resume playback state");

    reset(); AsyncWebServerRequest multipart;
    FPPDiscovery.ProcessFile(&multipart, "first.xlz", 0, bytes.data(), 100, true);
    require(FileMgr.IsUploadInProgress() && !InputMgr.active && OutputMgr.paused, "multipart first file resumed playback before request completed");
    FPPDiscovery.ProcessFile(&multipart, "second.xlz", 0, bytes.data(), 75, false);
    require(FileMgr.IsUploadInProgress() && !InputMgr.active && OutputMgr.paused, "multipart second file reused a live output buffer");
    FPPDiscovery.ProcessFile(&multipart, "second.xlz", 75, bytes.data() + 75, 1725, true);
    require(sd.files.at("first.xlz") == std::vector<byte>(bytes.begin(), bytes.begin() + 100) && sd.files.at("second.xlz") == bytes && gUnzipFiles.scans == 2, "multipart request corrupted or failed to queue an individual archive");
    require(FileMgr.IsUploadInProgress() && !InputMgr.active && OutputMgr.paused, "multipart final file resumed playback before response completion");
    FPPDiscovery.FinishFileUpload(&multipart);
    require(!FileMgr.IsUploadInProgress() && InputMgr.active && !OutputMgr.paused, "multipart completion did not release playback/upload hold");

    reset();
    AsyncWebServerRequest interrupted;
    FPPDiscovery.ProcessFile(&interrupted, "interrupted.xlz", 0, bytes.data(), 20, false);
    interrupted.disconnect();
    require(!FileMgr.IsUploadInProgress() && !sd.files.count("interrupted.xlz"), "disconnect left partial upload or active state");
    require(gUnzipFiles.scans == 0 && InputMgr.active && !OutputMgr.paused, "disconnect queued invalid archive or left outputs paused");

    reset();
    AsyncWebServerRequest completed;
    FPPDiscovery.ProcessFile(&completed, "completed.xlz", 0, bytes.data(), bytes.size(), true);
    completed.disconnect();
    require(sd.files.at("completed.xlz") == bytes, "disconnect deleted a successfully stored archive");
    require(gUnzipFiles.scans == 1 && !FileMgr.IsUploadInProgress() && InputMgr.active && !OutputMgr.paused, "completed disconnect left extraction or playback paused");

    for (const auto& invalid : {"../seq.xlz", "C:seq.xlz", "folder/seq.xlz", "folder\\seq.xlz", ".unzip.tmp", "seq.fseq.unzip.bak", "", "..", " seq.xlz", "seq.xlz ", "seq.xlz.", "seq.xlz. ", " .unzip.tmp", " .unzip.tmp.", "seq.fseq.unzip.bak. ", "seq\x7f.xlz"}) {
        reset(); AsyncWebServerRequest bad;
        FPPDiscovery.ProcessFile(&bad, invalid, 0, bytes.data(), 20, true);
        require(bad.response == 400 && sd.files.empty() && !FileMgr.IsUploadInProgress(), "unsafe filename accepted");
    }
    reset(); AsyncWebServerRequest conflict;
    gUnzipFiles.inUse.insert("active.xlz"); sd.files["active.xlz"] = {7};
    FPPDiscovery.ProcessFile(&conflict, "active.xlz", 0, bytes.data(), 20, true);
    require(conflict.response == 409 && sd.files.at("active.xlz") == std::vector<byte>{7} && gUnzipFiles.uploadReservations == 1 && FPPDiscovery.stops == 0 && !FileMgr.IsUploadInProgress(), "failed upload reservation altered the extracted file or upload/playback state");
    FPPDiscovery.ProcessFile(&conflict, "active.xlz", 20, bytes.data(), 20, true);
    require(conflict.response == 409, "subsequent rejected fragment replaced conflict response");

    reset(); AsyncWebServerRequest raw;
    raw.params["path"].text = "uploadFile"; raw.params["filename"].text = "raw.xlz";
    FPPDiscovery.ProcessBody(&raw, bytes.data(), 100, 0, 1800);
    require(FileMgr.IsUploadInProgress() && !gUnzipFiles.scans, "raw body finalized prematurely");
    FPPDiscovery.ProcessBody(&raw, bytes.data() + 100, 1700, 100, 1800);
    require(sd.files.at("raw.xlz") == bytes && gUnzipFiles.scans == 1, "raw FPP body upload failed");
    require(FileMgr.IsUploadInProgress(), "raw upload did not hold response phase");
    FPPDiscovery.ProcessPOST(&raw);
    require(raw.response == 200 && raw.body.indexOf("pending") >= 0 && FPPDiscovery.metadataReads == 0, "archive upload parsed ZIP data as FSEQ metadata");
    require(sd.files.at("raw.xlz") == bytes, "acknowledgment modified stored archive");
    require(!FileMgr.IsUploadInProgress(), "raw upload response did not release extraction");
    raw.disconnect();
    require(sd.files.at("raw.xlz") == bytes && !FileMgr.IsUploadInProgress(), "normal response disconnect deleted the stored archive or retained its upload reservation");
    reset(); AsyncWebServerRequest sequence;
    sequence.params["path"].text = "uploadFile"; sequence.params["filename"].text = "plain.fseq";
    FPPDiscovery.ProcessBody(&sequence, bytes.data(), 1800, 0, 1800);
    FPPDiscovery.ProcessPOST(&sequence);
    require(sequence.response == 200 && FPPDiscovery.metadataReads == 1 && !FileMgr.IsUploadInProgress(), "plain FSEQ response metadata behavior changed");
    AsyncWebServerRequest rejected;
    rejected.response = 409;
    FPPDiscovery.ProcessPOST(&rejected);
    require(rejected.response == 409 && FPPDiscovery.metadataReads == 1, "POST completion overwrote rejected upload response");
    AsyncWebServerRequest malformed;
    FPPDiscovery.ProcessBody(&malformed, bytes.data(), 1, 0, 1);
    require(malformed.response == 404, "missing raw route parameters accepted");
    reset(); AsyncWebServerRequest oversized;
    oversized.params["path"].text = "uploadFile"; oversized.params["filename"].text = "oversized.xlz";
    FPPDiscovery.ProcessBody(&oversized, bytes.data(), 1, 0, 2);
    FPPDiscovery.ProcessBody(&oversized, bytes.data(), 2, 1, 2);
    require(oversized.response == 400 && !FileMgr.IsUploadInProgress() && !sd.files.count("oversized.xlz"), "raw body beyond declared payload length accepted");
}

static void sequenceMetadata()
{
    for (const auto& filename : {"stored.xlz", "STORED.XLZ", "stored.zip", "STORED.ZIP"}) {
        reset();
        sd.files[filename] = payload(1800);
        AsyncWebServerRequest request;
        request.urlText = String("/api/sequence/") + filename + "/meta";
        FPPDiscovery.ProcessGET(&request);
        require(request.response == 404 && FPPDiscovery.metadataReads == 0 && FPPDiscovery.stops == 0 && !FileMgr.isOpen, "archive metadata request parsed ZIP bytes or interrupted playback");
        require(sd.files.count(filename) == 1, "archive metadata request removed the uploaded archive");
    }
    reset();
    sd.files["plain.fseq"] = payload(1800);
    AsyncWebServerRequest request;
    request.urlText = "/api/sequence/plain.fseq/meta";
    FPPDiscovery.ProcessGET(&request);
    require(request.response == 200 && FPPDiscovery.metadataReads == 1 && !FileMgr.isOpen, "extracted sequence metadata became unavailable");
}

static void storageCapacity()
{
    reset(); c_FileMgr::SdInfo info;
    FileMgr.GetSdInfo(info);
    require(info.MaxSize == 4'096'000 && info.Available == 3'072'000 && info.Used == 1'024'000, "SD free-cluster accounting incorrect");
    sd.freeClusters = UINT32_MAX;
    FileMgr.GetSdInfo(info);
    require(info.Available == 0 && info.Used == info.MaxSize, "invalid cluster count permitted extraction");
    FileMgr.SdCardInstalled = false;
    FileMgr.GetSdInfo(info);
    require(info.Available == 0 && info.MaxSize == 0 && info.Used == 0, "missing SD retained stale capacity");
}

int main()
{
    try {
        bufferingAndFinalization();
        replacementFaults();
        fppRequests();
        sequenceMetadata();
        storageCapacity();
        std::cout << "Production upload/storage methods: " << checks << " assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
