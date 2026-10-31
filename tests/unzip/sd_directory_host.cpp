// Real SdFat filesystem regression: no board, SD card, or fake file map.
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "FsLib/FsLib.h"

using String = std::string;
static unsigned directoryWarnings = 0;
static unsigned directoryErrors = 0;
static void FeedWDT() {}
static String int64String(uint64_t value) { return std::to_string(value); }
static void captureLog(const String& message) {
    if (message.find("WARNING:") == 0) ++directoryWarnings;
    if (message.find("ERROR:") == 0) ++directoryErrors;
}
class c_FileMgr {
 public:
    bool OpenNextSdEntry(FsFile & Entry, FsFile & Directory);
    void GetDriverName(String& name) { name = "FileMgr"; }
} FileMgr;
// Keep the logger's instance-method requirement so a file-local helper cannot
// accidentally pass this host test while failing the controller build.
#define logcon(message) do { String name; GetDriverName(name); captureLog(message); } while (false)
#include "sd_directory_methods.inc"

static unsigned checks = 0;
static void require(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}

// Sparse sectors keep the formatter's minimum FAT volume inexpensive.
class RamCard : public FsBlockDeviceInterface {
 public:
    std::map<Sector_t, std::vector<uint8_t>> sectors;
    Sector_t capacity;
    Sector_t failedReadSector = Sector_t(-1);
    explicit RamCard(Sector_t value) : capacity(value) {}
    bool isBusy() override { return false; }
    Sector_t sectorCount() override { return capacity; }
    bool syncDevice() override { return true; }
    bool readSector(Sector_t sector, uint8_t* data) override {
        if (sector >= sectorCount() || sector == failedReadSector) return false;
        auto found = sectors.find(sector);
        if (found == sectors.end()) std::memset(data, 0, 512);
        else std::memcpy(data, found->second.data(), 512);
        return true;
    }
    bool readSectors(Sector_t sector, uint8_t* data, size_t count) override {
        for (size_t i = 0; i < count; ++i)
            if (!readSector(sector + Sector_t(i), data + i * 512)) return false;
        return true;
    }
    bool writeSector(Sector_t sector, const uint8_t* data) override {
        if (sector >= sectorCount()) return false;
        sectors[sector] = std::vector<uint8_t>(data, data + 512);
        return true;
    }
    bool writeSectors(Sector_t sector, const uint8_t* data, size_t count) override {
        for (size_t i = 0; i < count; ++i)
            if (!writeSector(sector + Sector_t(i), data + i * 512)) return false;
        return true;
    }
};

static std::vector<std::string> scan(FsVolume& sd, bool resetAfterOpen, bool recover = false) {
    require(sd.chdir(), "Cannot change to root");
    FsFile root = sd.open("/", O_READ);
    require(bool(root), "Cannot open root");
    if (resetAfterOpen) require(sd.chdir(), "Cannot reset working directory");
    FsFile entry;
    std::vector<std::string> names;
    while (recover ? FileMgr.OpenNextSdEntry(entry, root) : entry.openNext(&root, O_READ)) {
        if (!entry.isDirectory() && !entry.isHidden() && entry.size()) {
            char name[256]{};
            require(entry.getName(name, sizeof(name)) != 0, "Cannot read directory filename");
            names.emplace_back(name);
        }
        entry.close();
    }
    require(!root.getError(), "Directory read error");
    root.close();
    return names;
}

static void exerciseFat(Sector_t capacity) {
        directoryWarnings = directoryErrors = 0;
        RamCard card(capacity);
        uint8_t scratch[512]{};
        FatFormatter format;
        require(format.format(&card, scratch), "Cannot format RAM card");
        FsVolume sd;
        require(sd.begin(&card), "Cannot mount FAT volume");
        FsFile emptyRoot = sd.open("/", O_READ);
        FsFile emptyEntry;
        require(!FileMgr.OpenNextSdEntry(emptyEntry, emptyRoot), "Empty root returned a directory entry");
        require(emptyRoot.curPosition() == 32, "End marker was scanned beyond the first empty record");
        require(directoryWarnings == 0 && directoryErrors == 0, "Ordinary EOF was reported as corruption");
        emptyRoot.close();
        for (const char* name : {"Broken Directory Test.fseq", "ImperialMarch.xlz", "same.fseq", "Another Long Archive.xlz"}) {
            FsFile output;
            require(output.open(name, O_WRITE | O_CREAT), "Cannot create uploaded filename");
            const std::string payload(178559, 'x');
            require(output.write(payload.data(), payload.size()) == payload.size(), "Incomplete payload write");
            require(output.sync(), "Cannot sync payload");
            require(output.close(), "Cannot close payload");
            FsFile verify;
            require(verify.open(name, O_READ), "Cannot reopen uploaded filename");
            require(verify.size() == payload.size(), "Reopened payload size mismatch");
            verify.close();
        }
        for (bool resetAfterOpen : {false, true}) {
            const auto names = scan(sd, resetAfterOpen);
            require(names.size() == 4, "Directory scan lost uploaded files");
            require(names[1] == "ImperialMarch.xlz", "Long uploaded filename missing from root");
            require(names[2] == "same.fseq", "FSEQ missing from root");
            require(names[3] == "Another Long Archive.xlz", "Second long archive missing from root");
        }
        // Change only the checksum of an unrelated LFN entry before the archive.
        // Direct pathname lookup can still find subsequent valid files, while
        // SdFat openNext stops at this malformed entry without a read error.
        FsFile root = sd.open("/", O_READ);
        uint8_t firstEntry[32]{};
        require(root.read(firstEntry, sizeof(firstEntry)) == sizeof(firstEntry), "Cannot read root entry");
        require(firstEntry[11] == 0x0f, "Fixture does not begin with LFN record");
        root.close();
        sd.end();
        bool corrupted = false;
        Sector_t rootSector = Sector_t(-1);
        for (auto& sector : card.sectors) {
            if (std::memcmp(sector.second.data(), firstEntry, sizeof(firstEntry)) == 0) {
                sector.second[13] ^= 1;
                corrupted = true;
                rootSector = sector.first;
                break;
            }
        }
        require(corrupted, "Cannot find root LFN on RAM card");
        require(sd.begin(&card), "Cannot remount altered RAM card");
        FsFile verify = sd.open("ImperialMarch.xlz", O_READ);
        require(bool(verify) && verify.size() == 178559, "Unrelated damaged LFN broke direct archive lookup");
        verify.close();
        require(scan(sd, true).empty(), "Malformed LFN did not reproduce empty directory list");
        const auto originalSectors = card.sectors;
        const auto recovered = scan(sd, true, true);
        require(recovered.size() == 3 && recovered[0] == "ImperialMarch.xlz" && recovered[1] == "same.fseq", "Recovery failed to discover valid files after malformed LFN");
        require(directoryWarnings == 1 && directoryErrors == 0, "Malformed directory entry was not diagnosed once");
        require(originalSectors == card.sectors, "Directory recovery modified SD bytes");
        sd.end();
        require(sd.begin(&card), "Cannot remount for directory read fault");
        card.failedReadSector = rootSector;
        FsFile unreadableRoot = sd.open("/", O_READ);
        FsFile unreadableEntry;
        require(!FileMgr.OpenNextSdEntry(unreadableEntry, unreadableRoot), "Directory I/O failure was ignored");
        require(directoryErrors == 1 && directoryWarnings == 1, "Directory I/O failure was retried as a malformed name");
        unreadableRoot.close();
}

int main() {
    try {
        exerciseFat(131072); // 64 MiB FAT16.
        exerciseFat(16777216); // 8 GiB FAT32; sparse storage avoids a large allocation.
        std::cout << "Real SdFat directory regression passed: " << checks << " assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
