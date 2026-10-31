/*
* UnzipFiles.cpp - Output Management class
*
* Project: ESPixelStick - An ESP8266 / ESP32 and E1.31 based pixel driver
* Copyright (c) 2021, 2025 Shelby Merrick
* http://www.forkineye.com
*
*  This program is provided free for you to use in any way that you wish,
*  subject to the laws and regulations where you are using it.  Due diligence
*  is strongly suggested before using this code.  Please give credit where due.
*
*  The Author makes no warranty of any kind, express or implied, with regard
*  to this program or the documentation contained in this document.  The
*  Author shall not be liable in any event for incidental or consequential
*  damages in connection with, or arising out of, the furnishing, performance
*  or use of these programs.
*
*/
#ifdef SUPPORT_UNZIP

#include "UnzipFiles.hpp"
#include "FileMgr.hpp"
#include "network/NetworkMgr.hpp"
#include "input/InputMgr.hpp"
#include <TimeLib.h>
#ifdef SUPPORT_OLED
#include "service/DisplayOLED.h"
#endif
//
// Callback functions needed by the unzipLIB to access a file system
// The library has built-in code for memory-to-memory transfers, but needs
// these callback functions to allow using other storage media
//
void * _OpenZipFile(const char *FileName, int32_t *size)
{
    return gUnzipFiles.OpenZipFile(FileName, size);
}

void _CloseZipFile(void *p)
{
    gUnzipFiles.CloseZipFile(p);
}

int32_t _ReadZipFile(void *p, uint8_t *buffer, int32_t length)
{
    return gUnzipFiles.ReadZipFile(p, buffer, length);
}

int32_t _SeekZipFile(void *p, int32_t position, int iType)
{
    return gUnzipFiles.SeekZipFile(p, position, iType);
} // _SeekZipFile

//-----------------------------------------------------------------------------
UnzipFiles::UnzipFiles()
{
#ifdef ARDUINO_ARCH_ESP32
    StatusMutex = xSemaphoreCreateMutex();
#endif
}

UnzipFiles::~UnzipFiles()
{
    free(pOutputBuffer);
#ifdef ARDUINO_ARCH_ESP32
    if (StatusMutex) vSemaphoreDelete(StatusMutex);
#endif
}

void UnzipFiles::LockStatus()
{
#ifdef ARDUINO_ARCH_ESP32
    if (StatusMutex) xSemaphoreTake(StatusMutex, portMAX_DELAY);
#endif
}

void UnzipFiles::UnlockStatus()
{
#ifdef ARDUINO_ARCH_ESP32
    if (StatusMutex) xSemaphoreGive(StatusMutex);
#endif
}

namespace
{
    bool IsArchive(String Name)
    {
        Name.toLowerCase();
        return Name.endsWith(".zip") || Name.endsWith(".xlz");
    }

    bool SameSdName(String Left, String Right)
    {
        if (Left.startsWith("/")) Left.remove(0, 1);
        if (Right.startsWith("/")) Right.remove(0, 1);
        // SdFat ignores leading spaces and trailing spaces/dots in path names.
        while (Left.startsWith(" ")) Left.remove(0, 1);
        while (Right.startsWith(" ")) Right.remove(0, 1);
        while (Left.endsWith(" ") || Left.endsWith(".")) Left.remove(Left.length() - 1);
        while (Right.endsWith(" ") || Right.endsWith(".")) Right.remove(Right.length() - 1);
        return Left.equalsIgnoreCase(Right);
    }

    bool SafeEntryName(const String & Name)
    {
        // ZIP paths use '/', and may never escape the SD root.
        if (Name.isEmpty() || Name.startsWith("/") ||
            Name.indexOf('\\') >= 0 || Name.indexOf(':') >= 0) return false;
        unsigned Start = 0;
        for (unsigned Index = 0; Index <= Name.length(); ++Index)
        {
            if (Index == Name.length() || Name[Index] == '/')
            {
                // A final slash is allowed for a directory entry.
                if (Start == Index) return Index == Name.length() && Index != 0;
                if (Name[Start] == ' ' || Name[Index - 1] == ' ' || Name[Index - 1] == '.') return false;
                Start = Index + 1;
            }
        }
        return true;
    }
}

void UnzipFiles::Run()
{
    // Upload callbacks can request discovery from the asynchronous server task.
    ScanRequested.store(true);
}

void UnzipFiles::DiscoverArchives()
{
    std::vector<String> Names;
    FileMgr.GetListOfSdFiles(Names);
    for (const auto & Name : Names)
    {
        if (!IsArchive(Name) || (ArchiveOpen && SameSdName(Name, CurrentArchive))) continue;
        bool Queued = false;
        for (const auto & Pending : PendingArchives)
        {
            if (SameSdName(Name, Pending)) { Queued = true; break; }
        }
        if (!Queued)
        {
            // A fresh batch keeps completion visible until new work arrives.
            if (PendingArchives.empty() && !ArchiveOpen)
            {
                ++JobId;
                FilesExtracted = ArchivesCompleted = ArchivesFailed = 0;
                BytesWritten = TotalBytes = 0;
                LastError.clear();
                CurrentEntry.clear();
            }
            PendingArchives.push_back(Name);
        }
    }
    if (!PendingArchives.empty() && !ArchiveOpen)
    {
        CurrentArchive = PendingArchives.front();
        CurrentState = State::Waiting;
    }
}

void UnzipFiles::Poll()
{
    // Never discover a partially uploaded archive or compete with upload buffers.
    if (!FileMgr.SdCardIsInstalled())
    {
        StatusGuard Guard(*this);
        if (!PendingArchives.empty() || ArchiveOpen)
        {
            LastError = CurrentArchive + F(": SD card removed during extraction");
            CloseArchive();
            ++ArchivesFailed;
            PendingArchives.clear();
            UpdateTerminalState();
        }
        return;
    }
    if (FileMgr.IsUploadInProgress()) return;
    StatusGuard Guard(*this);
    // Upload reservation uses this same guard, so opening an archive and
    // starting an upload cannot pass each other's activity checks.
    if (FileMgr.IsUploadInProgress()) return;
    if (ScanRequested.exchange(false)) DiscoverArchives();
    if (PendingArchives.empty()) return;

    if (!NetworkMgr.IsConnected())
    {
        CurrentState = State::Waiting;
        return;
    }
    CurrentState = State::Extracting;
    if (!InputsPaused)
    {
        // Close local playback files before any destination can be replaced.
        // This pause reason composes with test mode and upload/service pauses.
        InputMgr.SetFileExtractionPaused(true);
        InputsPaused = true;
    }
    FeedWDT();
    if (!ArchiveOpen)
    {
        OpenArchive();
    }
    else if (EntryOpen)
    {
        ReadEntry();
    }
    else if (AdvanceEntry)
    {
        AdvanceEntry = false;
        const int Code = zip.gotoNextFile();
        if (Code == UNZ_END_OF_LIST_OF_FILE) FinishArchive();
        else if (Code != UNZ_OK) Fail(F("Cannot read the next archive entry"));
    }
    else
    {
        OpenEntry();
    }
}

void UnzipFiles::OpenArchive()
{
    CurrentArchive = PendingArchives.front();
    CurrentEntry.clear();
    BytesWritten = TotalBytes = ArchiveFileCount = 0;
    AdvanceEntry = false;
    String LowerName = CurrentArchive;
    LowerName.toLowerCase();
    IsXlightsArchive = LowerName.endsWith(".xlz");

    // Allocate only while extracting; never consume heap before network startup.
    BufferSize = 4096;
    if (!pOutputBuffer) pOutputBuffer = static_cast<uint8_t *>(malloc(BufferSize));
    if (!pOutputBuffer) { Fail(F("Not enough memory to extract the archive")); return; }
    logcon(String(F("Unzip file: '")) + CurrentArchive + "'");
    if (zip.openZIP(CurrentArchive.c_str(), _OpenZipFile, _CloseZipFile, _ReadZipFile, _SeekZipFile) != UNZ_OK)
    {
        Fail(F("Cannot open ZIP archive"));
        return;
    }
    ArchiveOpen = true;
    if (zip.gotoFirstFile() != UNZ_OK)
    {
        Fail(F("Archive is empty or invalid"));
        return;
    }
#ifdef SUPPORT_OLED
    OLED.ShowToast("DECOMPRESSING", CurrentArchive);
#endif
}

void UnzipFiles::OpenEntry()
{
    char Name[256] = {0};
    unz_file_info Info = {};
    if (zip.getFileInfo(&Info, Name, sizeof(Name), nullptr, 0, nullptr, 0) != UNZ_OK ||
        Info.size_filename == 0 || Info.size_filename >= sizeof(Name))
    {
        Fail(F("Invalid or too long archive filename"));
        return;
    }
    String EntryName(Name);
    if (EntryName.length() != Info.size_filename)
    {
        Fail(F("Invalid archive filename"));
        return;
    }
    for (unsigned Index = 0; Index < EntryName.length(); ++Index)
    {
        if (static_cast<uint8_t>(EntryName[Index]) < 32 || EntryName[Index] == 127)
        {
            Fail(F("Invalid archive filename"));
            return;
        }
    }
    if (!SafeEntryName(EntryName)) { Fail(F("Unsafe archive filename")); return; }
    if (EntryName.endsWith("/")) { AdvanceEntry = true; return; }

    CurrentEntry = EntryName;
    if (IsXlightsArchive)
    {
        String LowerEntry = EntryName;
        LowerEntry.toLowerCase();
        if (!LowerEntry.endsWith(".fseq") || ArchiveFileCount != 0)
        {
            Fail(F("An xLights archive must contain exactly one .fseq file"));
            return;
        }
        // Write directly to a staged final name, including the upstream fix for
        // archives whose inner filename already matches the final sequence name.
        CurrentEntry = CurrentArchive.substring(0, CurrentArchive.length() - 4) + ".fseq";
    }
    else if (EntryName.indexOf('/') >= 0)
    {
        Fail(F("ZIP files must contain files in the root directory"));
        return;
    }
    String LowerTarget = CurrentEntry;
    LowerTarget.toLowerCase();
    if (SameSdName(CurrentEntry, CurrentArchive) || SameSdName(CurrentEntry, TempFileName) ||
        LowerTarget.endsWith(".unzip.bak") || IsArchive(CurrentEntry))
    {
        Fail(F("Archive contains a reserved filename"));
        return;
    }

    c_FileMgr::SdInfo SdInfo;
    FileMgr.GetSdInfo(SdInfo);
    if (uint64_t(Info.uncompressed_size) + 512 > SdInfo.Available)
    {
        Fail(F("Not enough free space on the SD card"));
        return;
    }
    BytesWritten = 0;
    TotalBytes = Info.uncompressed_size;
    FileMgr.DeleteSdFile(TempFileName);
    if (zip.openCurrentFile() != UNZ_OK)
    {
        Fail(F("Cannot open the compressed entry"));
        return;
    }
    EntryOpen = true;
    if (!FileMgr.OpenSdFile(TempFileName, c_FileMgr::FileMode::FileWrite, OutputFile, -1))
    {
        Fail(F("Cannot create the extracted file on the SD card"));
    }
}

void UnzipFiles::ReadEntry()
{
    // One bounded chunk per loop keeps network, GUI and OLED polling alive.
    const int Read = zip.readCurrentFile(pOutputBuffer, BufferSize);
    if (Read < 0) { Fail(F("Decompression failed (damaged archive)")); return; }
    if (Read > 0)
    {
        if (uint64_t(BytesWritten) + Read > TotalBytes ||
            FileMgr.WriteSdFile(OutputFile, pOutputBuffer, Read) != uint32_t(Read))
        {
            Fail(F("Could not write the extracted file"));
            return;
        }
        BytesWritten += Read;
        return;
    }
    const int CloseResult = zip.closeCurrentFile();
    EntryOpen = false;
    FileMgr.CloseSdFile(OutputFile);
    if (CloseResult != UNZ_OK || BytesWritten != TotalBytes ||
        FileMgr.GetSdFileSize(TempFileName) != TotalBytes)
    {
        Fail(F("Extracted file failed size or CRC verification"));
        return;
    }
    if (!IsXlightsArchive && !FileMgr.ReplaceSdFile(TempFileName, CurrentEntry))
    {
        Fail(F("Cannot replace the sequence file; previous file was preserved"));
        return;
    }
    if (!IsXlightsArchive) ++FilesExtracted;
    ++ArchiveFileCount;
    AdvanceEntry = true;
}

void UnzipFiles::CloseArchive()
{
    if (OutputFile != c_FileMgr::INVALID_FILE_HANDLE) FileMgr.CloseSdFile(OutputFile);
    if (EntryOpen) zip.closeCurrentFile();
    EntryOpen = false;
    if (ArchiveOpen) zip.closeZIP();
    ArchiveOpen = false;
    AdvanceEntry = false;
    free(pOutputBuffer);
    pOutputBuffer = nullptr;
    BufferSize = 0;
}

void UnzipFiles::UpdateTerminalState()
{
    if (!PendingArchives.empty())
    {
        CurrentArchive = PendingArchives.front();
        CurrentEntry.clear();
        BytesWritten = TotalBytes = 0;
        CurrentState = State::Waiting;
    }
    else
    {
        CurrentState = ArchivesFailed ? State::Error : State::Complete;
        FileMgr.BuildFseqList(false);
        if (InputsPaused)
        {
            InputMgr.SetFileExtractionPaused(false);
            InputsPaused = false;
        }
    }
}

void UnzipFiles::FinishArchive()
{
    if (ArchiveFileCount == 0) { Fail(F("Archive contains no sequence files")); return; }
    // Validate the whole single-sequence .xlz before committing its output.
    if (IsXlightsArchive)
    {
        if (!FileMgr.ReplaceSdFile(TempFileName, CurrentEntry))
        {
            Fail(F("Cannot replace the sequence file; previous file was preserved"));
            return;
        }
        ++FilesExtracted;
    }
    CloseArchive();
    // The source is removed only after every entry passed size, CRC and write checks.
    FileMgr.DeleteSdFile(CurrentArchive);
    ++ArchivesCompleted;
#ifdef SUPPORT_OLED
    OLED.ShowToast(" COMPLETED ", CurrentArchive);
#endif
    PendingArchives.erase(PendingArchives.begin());
    UpdateTerminalState();
}

void UnzipFiles::Fail(const String & Reason)
{
    LastError = CurrentArchive + ": " + Reason;
    logcon(String(F("Unzip failed: ")) + LastError);
    CloseArchive();
    FileMgr.DeleteSdFile(TempFileName);
    ++ArchivesFailed;
#ifdef SUPPORT_OLED
    OLED.ShowToast("UNZIP FAILED", CurrentArchive);
#endif
    if (!PendingArchives.empty()) PendingArchives.erase(PendingArchives.begin());
    UpdateTerminalState();
}

bool UnzipFiles::FileInUse(const String & FileName) const
{
    return ArchiveOpen && (SameSdName(FileName, CurrentArchive) ||
        SameSdName(FileName, CurrentEntry) || SameSdName(FileName, TempFileName) ||
        SameSdName(FileName, CurrentEntry + ".unzip.bak"));
}

bool UnzipFiles::IsFileInUse(const String & FileName)
{
    StatusGuard Guard(*this);
    return FileInUse(FileName);
}

bool UnzipFiles::TryBeginUpload(const String & FileName, std::atomic<bool> & UploadInProgress)
{
    StatusGuard Guard(*this);
    if (FileInUse(FileName)) return false;
    UploadInProgress.store(true);
    return true;
}

void UnzipFiles::GetStatus(JsonObject & Json)
{
    StatusGuard Guard(*this);
    JsonObject Status = Json["unzip"].to<JsonObject>();
    const char * Names[] = {"idle", "waiting", "extracting", "complete", "error"};
    Status["state"] = Names[static_cast<unsigned>(CurrentState)];
    Status["jobId"] = JobId;
    Status["isUnzipping"] = CurrentState == State::Extracting;
    Status["hasPending"] = !PendingArchives.empty() || ScanRequested.load();
    Status["fileName"] = CurrentArchive;
    Status["entryName"] = CurrentEntry;
    Status["bytesWritten"] = BytesWritten;
    Status["totalBytes"] = TotalBytes;
    Status["filesExtracted"] = FilesExtracted;
    Status["archivesCompleted"] = ArchivesCompleted;
    Status["archivesFailed"] = ArchivesFailed;
    Status["error"] = LastError;
}

//-----------------------------------------------------------------------------
void * UnzipFiles::OpenZipFile(const char *FileName, int32_t *size)
{
    *size = 0;
    ArchiveSize = SeekPosition = 0;
    c_FileMgr::FileId Handle = c_FileMgr::INVALID_FILE_HANDLE;
    if (!FileMgr.OpenSdFile(FileName, c_FileMgr::FileMode::FileRead, Handle, -1)) return nullptr;
    const uint64_t Length = FileMgr.GetSdFileSize(Handle);
    // unzipLIB uses signed 32-bit offsets, including its end-of-file seek.
    if (Length == 0 || Length > INT32_MAX)
    {
        FileMgr.CloseSdFile(Handle);
        return nullptr;
    }
    ArchiveSize = *size = static_cast<int32_t>(Length);
    return reinterpret_cast<void *>(static_cast<uintptr_t>(Handle));
}

void UnzipFiles::CloseZipFile(void *p)
{
    auto Handle = static_cast<c_FileMgr::FileId>(reinterpret_cast<uintptr_t>(((ZIPFILE *)p)->fHandle));
    FileMgr.CloseSdFile(Handle);
    ArchiveSize = SeekPosition = 0;
}

int32_t UnzipFiles::ReadZipFile(void *p, uint8_t *buffer, int32_t length)
{
    if (length <= 0 || SeekPosition < 0 || SeekPosition >= ArchiveSize) return 0;
    const int32_t ToRead = min(length, ArchiveSize - SeekPosition);
    const auto Handle = static_cast<c_FileMgr::FileId>(reinterpret_cast<uintptr_t>(((ZIPFILE *)p)->fHandle));
    const uint64_t Read = FileMgr.ReadSdFile(Handle, buffer, ToRead, SeekPosition);
    if (Read > uint32_t(ToRead)) return 0;
    SeekPosition += Read;
    return static_cast<int32_t>(Read);
}

int32_t UnzipFiles::SeekZipFile(void *p, int32_t position, int iType)
{
    (void)p;
    const int64_t Base = iType == SEEK_SET ? 0 : iType == SEEK_CUR ? SeekPosition : ArchiveSize;
    const int64_t Target = Base + position;
    if ((iType != SEEK_SET && iType != SEEK_CUR && iType != SEEK_END) || Target < 0 || Target > ArchiveSize)
        return -1;
    SeekPosition = static_cast<int32_t>(Target);
    return SeekPosition;
}

void UnzipFiles::DosDateToTmuDate(uint32_t DosDate, tmElements_t *ptm)
{
    // Extract DOS DATE (high 16 bits)
    uint16_t date = uint16_t(DosDate >> 16);

    uint16_t day   =  date        & 0x1F;       // 1–31
    uint16_t month = (date >> 5)  & 0x0F;       // 1–12
    uint16_t year  = (date >> 9)  & 0x7F;       // years since 1980

    // Extract DOS TIME (low 16 bits)
    uint16_t time  = uint16_t(DosDate & 0xFFFF);

    uint16_t hour   = (time >> 11) & 0x1F;      // 0–23
    uint16_t minute = (time >> 5)  & 0x3F;      // 0–59
    uint16_t second = (time & 0x1F) * 2;        // stored in 2-sec increments

    // DOS spec allows month 0 for "invalid" → clamp to 1
    if (month < 1) month = 1;
    if (month > 12) month = 12;

    if (day < 1) day = 1;
    if (day > 31) day = 31;

    if (hour > 23) hour = 23;
    if (minute > 59) minute = 59;
    if (second > 59) second = 59;

    // Convert the fields into TimeLib format
    ptm->Day    = (uInt)day;
    ptm->Month  = (uInt)month;                         // TimeLib expects 1–12
    ptm->Year   = (uInt)CalendarYrToTm(1980 + year);   // Convert to tmElements_t year

    ptm->Hour   = (uInt)hour;
    ptm->Minute = (uInt)minute;
    ptm->Second = (uInt)second;
/*
    // DEBUG_V(String("Year: ") + String(tmYearToCalendar(FileDate.Year)));
    // DEBUG_V(String("Month: ") + String(FileDate.Month));
    // DEBUG_V(String("Day: ") + String(FileDate.Day));
    // DEBUG_V(String("Hour: ") + String(FileDate.Hour));
    // DEBUG_V(String("Minute: ") + String(FileDate.Minute));
    // DEBUG_V(String("Second: ") + String(FileDate.Second));
*/
} // DosDateToTmuDate

UnzipFiles gUnzipFiles;

#endif // def SUPPORT_UNZIP
