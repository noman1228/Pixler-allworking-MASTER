#include "ESPixelStick.h"
#include "FileMgr.hpp"
#include "network/NetworkMgr.hpp"
#include "UnzipFiles.hpp"
#include "input/InputMgr.hpp"
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>

static std::string fixtureDirectory;
static unsigned assertions = 0;

static void require(bool condition, const std::string& message)
{
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

static std::vector<uint8_t> fixture(const std::string& filename)
{
    std::ifstream input(fixtureDirectory + "/" + filename, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read fixture " + filename);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

static void reset()
{
    gUnzipFiles.~UnzipFiles();
    FileMgr = c_FileMgr{};
    NetworkMgr.connected = false;
    InputMgr = c_InputMgr{};
    testClock = 0;
    testRebooting = false;
    new (&gUnzipFiles) UnzipFiles();
}

struct Status
{
    std::string state, file, entry, error;
    uint64_t written, total;
    unsigned extracted, completed, failed, job;
    bool active, pending;
};

static Status status()
{
    JsonDocument document;
    JsonObject root = document.to<JsonObject>();
    gUnzipFiles.GetStatus(root);
    JsonObject unzip = root["unzip"];
    return {
        unzip["state"] | "", unzip["fileName"] | "", unzip["entryName"] | "", unzip["error"] | "",
        unzip["bytesWritten"] | uint64_t(0), unzip["totalBytes"] | uint64_t(0),
        unzip["filesExtracted"] | 0U, unzip["archivesCompleted"] | 0U, unzip["archivesFailed"] | 0U,
        unzip["jobId"] | 0U, unzip["isUnzipping"] | false, unzip["hasPending"] | false
    };
}

static void poll()
{
    testClock += 1001;
    auto before = FileMgr.bytesWritten;
    gUnzipFiles.Poll();
    require(FileMgr.bytesWritten - before <= 4096, "A Poll wrote more than one 4096-byte extraction chunk");
}

static Status finish()
{
    for (unsigned attempt = 0; attempt < 5000; ++attempt) {
        poll();
        auto current = status();
        if (current.state == "complete" || current.state == "error") return current;
    }
    throw std::runtime_error("Extraction did not reach a terminal state");
}

static void add(const String& filename, const std::string& source = "")
{
    FileMgr.files[filename] = fixture(source.empty() ? std::string(filename) : source);
}

static void requireNoTemporaryFiles()
{
    for (const auto& file : FileMgr.files) {
        require(!file.first.endsWith(".unzip.tmp"), "Temporary output survived extraction cleanup");
    }
    require(FileMgr.handles.empty(), "Extraction leaked an SD file handle");
    require(!InputMgr.extractionPaused, "Extraction left normal inputs paused after completion or failure");
}

static void deferredAndCooperative()
{
    reset();
    add("same.xlz");
    gUnzipFiles.Run();
    for (unsigned count = 0; count < 3; ++count) poll();
    auto waiting = status();
    require(waiting.state == "waiting", "Archive was not reported as waiting for network");
    require(waiting.pending, "Waiting archive not marked pending");
    require(FileMgr.opens == 0 && FileMgr.bytesWritten == 0, "Extraction touched archive before network connection");
    require(FileMgr.files.count("same.xlz") == 1, "Pending archive was deleted");
    require(!InputMgr.extractionPaused, "Waiting for initial network connection paused normal inputs");

    NetworkMgr.connected = true;
    bool observedProgress = false;
    uint64_t previousWritten = 0;
    for (unsigned count = 0; count < 500; ++count) {
        poll();
        auto current = status();
        if (current.state == "complete") break;
        if (current.state == "extracting" && current.written > 0) {
            observedProgress = true;
            require(current.active, "Extracting state omitted active flag");
            require(current.written >= previousWritten && current.written <= current.total, "Live byte progress was inconsistent");
            previousWritten = current.written;
        }
    }
    auto complete = status();
    require(complete.state == "complete", "Deferred archive did not finish");
    require(observedProgress, "Large archive completed without observable intermediate progress");
    require(complete.extracted == 1 && complete.completed == 1 && complete.failed == 0, "Successful status counters incorrect");
    require(FileMgr.files.at("same.fseq") == fixture("payload.bin"), "Matching inner/outer FSEQ name lost data");
    require(!FileMgr.files.count("same.xlz"), "Successful archive was not removed");
    require(FileMgr.rebuilds > 0, "Extracted file list was not refreshed");
    require(!testRebooting, "Extraction requested an unnecessary reboot");
    requireNoTemporaryFiles();
}

static void xlightsNamingAndCompression()
{
    for (const auto& filename : {"renamed.xlz", "UPPER.XLZ", "stored.xlz", "nested.xlz"}) {
        reset();
        add(filename);
        NetworkMgr.connected = true;
        gUnzipFiles.Run();
        auto complete = finish();
        String target = String(filename).substring(0, unsigned(std::strlen(filename) - 4)) + ".fseq";
        require(complete.state == "complete", std::string("Failed XLZ fixture ") + filename + ": " + complete.error);
        require(FileMgr.files.count(target) && FileMgr.files.at(target) == fixture("payload.bin"), std::string("XLZ did not map to outer FSEQ filename: ") + filename + " => " + target);
        require(FileMgr.files.size() == 1, "XLZ left an inner-name or archive file behind");
        requireNoTemporaryFiles();
    }
}

static void genericZipAndDirectories()
{
    reset();
    add("batch.zip");
    NetworkMgr.connected = true;
    gUnzipFiles.Run();
    auto complete = finish();
    auto second = fixture("payload.bin");
    std::reverse(second.begin(), second.end());
    require(complete.state == "complete" && complete.extracted == 2, "ZIP did not extract all entries");
    require(FileMgr.files.at("first.fseq") == fixture("payload.bin"), "First ZIP entry changed");
    require(FileMgr.files.at("second.fseq") == second, "Second ZIP entry changed");
    requireNoTemporaryFiles();

    reset();
    add("directory.zip");
    NetworkMgr.connected = true;
    gUnzipFiles.Run();
    complete = finish();
    require(complete.state == "complete", "ZIP directory entry should be skipped");
    require(FileMgr.files.at("normal.fseq") == fixture("payload.bin"), "ZIP directory prevented subsequent file extraction");
    require(!FileMgr.files.count("folder/"), "ZIP directory became a file");
    requireNoTemporaryFiles();
}

static void failurePreservesOriginals()
{
    for (const auto& filename : {"crc-bad.xlz", "deflate-bad.xlz", "not-a-zip.xlz"}) {
        reset();
        add(filename);
        String target = String(filename).substring(0, unsigned(std::strlen(filename) - 4)) + ".fseq";
        std::vector<uint8_t> original = {'o', 'l', 'd', '-', 's', 'e', 'q'};
        FileMgr.files[target] = original;
        NetworkMgr.connected = true;
        gUnzipFiles.Run();
        auto failed = finish();
        require(failed.state == "error" && failed.failed == 1, std::string("Bad archive did not fail: ") + filename);
        require(!failed.error.empty(), "Failure omitted a diagnostic");
        require(FileMgr.files.count(filename), "Failed archive was deleted");
        require(FileMgr.files.at(target) == original, "Failed extraction replaced existing sequence");
        requireNoTemporaryFiles();
    }

    for (unsigned kind = 0; kind < 3; ++kind) {
        reset();
        add("same.xlz");
        std::vector<uint8_t> original = {'o', 'l', 'd'};
        FileMgr.files["same.fseq"] = original;
        if (kind == 0) FileMgr.shortWrite = true;
        if (kind == 1) FileMgr.failRename = true;
        if (kind == 2) FileMgr.available = 10;
        NetworkMgr.connected = true;
        gUnzipFiles.Run();
        auto failed = finish();
        require(failed.state == "error", "SD fault did not report failure");
        require(FileMgr.files.count("same.xlz"), "SD fault removed recoverable archive");
        require(FileMgr.files.at("same.fseq") == original, "SD fault lost existing target");
        requireNoTemporaryFiles();
    }

    reset();
    add("same.xlz");
    const std::vector<uint8_t> original = {'o', 'l', 'd'};
    FileMgr.files["same.fseq"] = original;
    NetworkMgr.connected = true;
    gUnzipFiles.Run();
    for (unsigned attempt = 0; attempt < 10 && status().written == 0; ++attempt) poll();
    require(status().written > 0, "Read-fault test did not reach extraction");
    FileMgr.failRead = true;
    auto failed = finish();
    require(failed.state == "error" && !failed.active && !failed.pending, "Mid-stream SD read failure did not finish with visible error");
    require(FileMgr.files.count("same.xlz") && FileMgr.files.at("same.fseq") == original, "Mid-stream SD read failure lost recoverable files");
    requireNoTemporaryFiles();
}

static void rejectUnsafeEntries()
{
    for (const auto& filename : {"traversal.zip", "nested.zip", "upper-backup.zip", "nul-name.zip", "fat-alias.zip", "leading-space.zip"}) {
        reset();
        add(filename);
        NetworkMgr.connected = true;
        gUnzipFiles.Run();
        auto failed = finish();
        require(failed.state == "error", std::string("Unsafe ZIP entry was accepted: ") + filename);
        require(FileMgr.files.count(filename), "Unsafe archive was deleted");
        require(FileMgr.files.size() == 1, "Unsafe ZIP entry created an output file");
        requireNoTemporaryFiles();
    }
}

static void wholeXlightsArchiveValidation()
{
    reset();
    add("multiple.xlz");
    const std::vector<uint8_t> original = {'o', 'l', 'd'};
    FileMgr.files["multiple.fseq"] = original;
    NetworkMgr.connected = true;
    gUnzipFiles.Run();
    auto failed = finish();
    require(failed.state == "error", "XLZ with multiple sequences was accepted");
    require(failed.extracted == 0 && failed.completed == 0 && failed.failed == 1, "Invalid XLZ incorrectly reported committed output");
    require(FileMgr.files.at("multiple.fseq") == original, "First sequence in invalid XLZ replaced old sequence before full validation");
    require(FileMgr.files.count("multiple.xlz"), "Invalid multi-sequence XLZ was deleted");
    requireNoTemporaryFiles();
}

static void pauseResumeAndInUseProtection()
{
    reset();
    add("same.xlz");
    NetworkMgr.connected = true;
    gUnzipFiles.Run();
    for (unsigned attempt = 0; attempt < 10 && status().written == 0; ++attempt) poll();
    require(InputMgr.extractionPaused, "Extraction did not pause normal inputs before opening files");
    auto beforePause = status();
    require(beforePause.state == "extracting" && beforePause.written > 0, "Pause test did not reach active extraction");
    for (const auto& name : {"same.xlz", "/same.xlz", "SAME.XLZ", "same.fseq", "/SAME.FSEQ", ".unzip.tmp", "same.fseq.unzip.bak", "/SAME.FSEQ.UNZIP.BAK", "same.xlz ", " same.fseq", ".unzip.tmp.", "same.fseq.UNZIP.BAK "}) {
        require(gUnzipFiles.IsFileInUse(name), std::string("Active extraction filename not protected: ") + name);
    }
    require(!gUnzipFiles.IsFileInUse("unrelated.fseq"), "Unrelated file was marked in use");
    NetworkMgr.connected = false;
    for (unsigned attempt = 0; attempt < 5; ++attempt) poll();
    auto disconnected = status();
    require(disconnected.state == "waiting" && disconnected.written == beforePause.written, "Disconnected extraction did not pause with retained progress");
    require(gUnzipFiles.IsFileInUse("same.fseq"), "Network pause released active output protection");
    require(InputMgr.extractionPaused, "Network pause released the file playback guard");
    NetworkMgr.connected = true;
    FileMgr.uploading = true;
    for (unsigned attempt = 0; attempt < 5; ++attempt) poll();
    require(status().written == beforePause.written, "Unrelated upload did not pause active extraction");
    FileMgr.uploading = false;
    auto complete = finish();
    require(complete.state == "complete" && FileMgr.files.at("same.fseq") == fixture("payload.bin"), "Paused extraction failed to resume intact");
    require(!gUnzipFiles.IsFileInUse("same.fseq"), "Completed extraction retained filename protection");
    requireNoTemporaryFiles();
}

static void sdRemovalReleasesInputs()
{
    reset();
    add("same.xlz");
    NetworkMgr.connected = true;
    gUnzipFiles.Run();
    for (unsigned attempt = 0; attempt < 10 && status().written == 0; ++attempt) poll();
    require(InputMgr.extractionPaused, "Removal fixture never acquired playback pause");
    FileMgr.installed = false;
    poll();
    require(status().state == "error" && !status().pending, "SD removal did not abort extraction");
    require(!InputMgr.extractionPaused, "SD removal stranded normal inputs in extraction pause");
    require(FileMgr.handles.empty(), "SD removal leaked archive handles");
}

static void reuploadSameNameAndSuffixMatching()
{
    reset();
    add("same.xlz");
    add("ignored.xlz.partial", "renamed.xlz");
    NetworkMgr.connected = true;
    gUnzipFiles.Run();
    auto complete = finish();
    require(complete.completed == 1 && FileMgr.files.count("ignored.xlz.partial"), "Archive discovery matched extension inside unfinished filename");
    auto previousJob = complete.job;
    FileMgr.files["same.xlz"] = fixture("stored.xlz");
    gUnzipFiles.Run();
    complete = finish();
    require(complete.state == "complete" && complete.job != previousJob && complete.completed == 1, "Repeated archive filename was ignored or retained stale counters");
    require(FileMgr.files.at("same.fseq") == fixture("payload.bin"), "Reuploaded archive failed to replace its prior output");
    requireNoTemporaryFiles();

    reset();
    add("same.xlz", "not-a-zip.xlz");
    NetworkMgr.connected = true;
    gUnzipFiles.Run();
    auto failed = finish();
    require(failed.state == "error", "Retry fixture did not report its initial failure");
    previousJob = failed.job;
    add("same.xlz");
    gUnzipFiles.Run();
    complete = finish();
    require(complete.state == "complete" && complete.job != previousJob && complete.failed == 0 && complete.error.empty(), "Reuploading repaired archive retained stale failure state");
    require(FileMgr.files.at("same.fseq") == fixture("payload.bin"), "Repaired same-name archive was not extracted");
    requireNoTemporaryFiles();
}

static void callbackBounds()
{
    reset();
    add("same.xlz");
    int32_t length = -1;
    ZIPFILE archive = {};
    archive.fHandle = gUnzipFiles.OpenZipFile("same.xlz", &length);
    require(archive.fHandle != nullptr && length == int32_t(FileMgr.files.at("same.xlz").size()), "ZIP open callback did not return actual file length");
    uint8_t bytes[8] = {};
    require(gUnzipFiles.SeekZipFile(&archive, -1, SEEK_SET) == -1, "Negative ZIP offset was accepted");
    require(gUnzipFiles.SeekZipFile(&archive, length + 1, SEEK_SET) == -1, "ZIP seek beyond file bounds was accepted");
    require(gUnzipFiles.SeekZipFile(&archive, 1, SEEK_END) == -1, "ZIP end-relative seek beyond file was accepted");
    require(gUnzipFiles.SeekZipFile(&archive, 0, 12345) == -1, "Unknown ZIP seek origin was accepted");
    require(gUnzipFiles.SeekZipFile(&archive, length - 2, SEEK_SET) == length - 2, "Valid ZIP offset was rejected");
    require(gUnzipFiles.ReadZipFile(&archive, bytes, 8) == 2, "ZIP read was not clamped at end of archive");
    require(gUnzipFiles.ReadZipFile(&archive, bytes, 8) == 0, "ZIP EOF read returned data");
    require(gUnzipFiles.SeekZipFile(&archive, -4, SEEK_CUR) == length - 4, "Current-relative ZIP seek failed");
    require(gUnzipFiles.SeekZipFile(&archive, 0, SEEK_SET) == 0, "ZIP rewind failed");
    require(gUnzipFiles.ReadZipFile(&archive, bytes, -1) == 0, "Negative ZIP read length was accepted");
    require(gUnzipFiles.ReadZipFile(&archive, bytes, 4) == 4 && std::memcmp(bytes, "PK\3\4", 4) == 0, "Invalid ZIP seek damaged the next valid read");
    gUnzipFiles.CloseZipFile(&archive);
    require(FileMgr.handles.empty(), "ZIP callback close leaked a file handle");
}

static void uploadsAndAdditionalArchives()
{
    reset();
    add("same.xlz");
    NetworkMgr.connected = true;
    FileMgr.uploading = true;
    gUnzipFiles.Run();
    for (unsigned count = 0; count < 5; ++count) poll();
    require(FileMgr.bytesWritten == 0 && FileMgr.opens == 0, "Archive extraction overlapped an active upload");
    FileMgr.uploading = false;
    auto complete = finish();
    require(complete.state == "complete", "Extraction did not resume after upload closed");
    unsigned previousJob = complete.job;
    add("renamed.xlz");
    gUnzipFiles.Run();
    complete = finish();
    require(complete.state == "complete", "Archive uploaded after initial batch was ignored");
    require(complete.job != previousJob, "New extraction batch did not advance job identity");
    require(FileMgr.files.at("renamed.fseq") == fixture("payload.bin"), "Newly uploaded archive data changed");
    requireNoTemporaryFiles();

    reset();
    add("same.xlz");
    NetworkMgr.connected = true;
    gUnzipFiles.Run();
    for (unsigned attempt = 0; attempt < 10 && status().written == 0; ++attempt) poll();
    unsigned activeJob = status().job;
    FileMgr.uploading = true;
    add("renamed.xlz");
    gUnzipFiles.Run();
    poll();
    FileMgr.uploading = false;
    complete = finish();
    require(complete.state == "complete" && complete.completed == 2 && complete.extracted == 2, "Newly uploaded archive did not join an active batch");
    require(complete.job == activeJob, "Adding to an active batch reset its progress identity");
    require(FileMgr.files.at("same.fseq") == fixture("payload.bin") && FileMgr.files.at("renamed.fseq") == fixture("payload.bin"), "Active batch upload lost one of its outputs");
    requireNoTemporaryFiles();

    reset();
    add("not-a-zip.xlz");
    add("same.xlz");
    NetworkMgr.connected = true;
    gUnzipFiles.Run();
    auto failed = finish();
    require(failed.state == "error" && failed.failed == 1 && failed.completed == 1, "One bad archive blocked or hid the rest of the queue");
    require(FileMgr.files.count("not-a-zip.xlz") && !FileMgr.files.count("same.xlz"), "Mixed queue removed wrong archive");
    require(FileMgr.files.at("same.fseq") == fixture("payload.bin"), "Valid archive after failed archive was not extracted");
    for (unsigned count = 0; count < 10; ++count) poll();
    require(status().failed == 1, "Failed archive was retried continuously");
    requireNoTemporaryFiles();
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    fixtureDirectory = argv[1];
    try {
        deferredAndCooperative();
        xlightsNamingAndCompression();
        genericZipAndDirectories();
        failurePreservesOriginals();
        rejectUnsafeEntries();
        wholeXlightsArchiveValidation();
        pauseResumeAndInUseProtection();
        sdRemovalReleasesInputs();
        reuploadSameNameAndSuffixMatching();
        callbackBounds();
        uploadsAndAdditionalArchives();
        std::cout << "PASS: cooperative extraction, real ZIP/XLZ fixtures, upload deferral, and SD/corruption faults (" << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
