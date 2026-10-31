#pragma once
/*
* UnzipFiles.hpp
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

#include "ESPixelStick.h"
#include <unzipLIB.h>
#include <TimeLib.h>
#include <atomic>
#include <vector>
#include "FileMgr.hpp"

class UnzipFiles
{
public:
    UnzipFiles ();
    virtual ~UnzipFiles ();

    // Discovery and extraction run from Poll(), after the network is ready.
    void    Run();
    void    Poll();
    void    GetStatus(JsonObject & json);
    bool    IsFileInUse(const String & FileName);
    bool    TryBeginUpload(const String & FileName, std::atomic<bool> & UploadInProgress);

    void *  OpenZipFile(const char *filename, int32_t *size);
    void    CloseZipFile(void *p);
    int32_t ReadZipFile(void *p, uint8_t *buffer, int32_t length);
    int32_t SeekZipFile(void *p, int32_t position, int iType);
    void    GetDriverName(String & value) {value = "Unzip";}

private:
    UNZIP       zip; // statically allocate the UNZIP structure (41K)
    uint8_t     *pOutputBuffer = nullptr;
    uint32_t    BufferSize = 0;
    int32_t     SeekPosition = 0;
    int32_t     ArchiveSize = 0;
    void        DosDateToTmuDate (uint32_t DosDate, tmElements_t * ptm);

    enum class State { Idle, Waiting, Extracting, Complete, Error };
    State       CurrentState = State::Idle;
    std::atomic<bool> ScanRequested {false};
    std::vector<String> PendingArchives;
    String      CurrentArchive;
    String      CurrentEntry;
    String      LastError;
    const String TempFileName = ".unzip.tmp";
    c_FileMgr::FileId OutputFile = c_FileMgr::INVALID_FILE_HANDLE;
    bool        ArchiveOpen = false;
    bool        EntryOpen = false;
    bool        AdvanceEntry = false;
    bool        IsXlightsArchive = false;
    bool        InputsPaused = false;
    uint32_t    BytesWritten = 0;
    uint32_t    TotalBytes = 0;
    uint32_t    FilesExtracted = 0;
    uint32_t    ArchiveFileCount = 0;
    uint32_t    ArchivesCompleted = 0;
    uint32_t    ArchivesFailed = 0;
    uint32_t    JobId = 0;

    void        DiscoverArchives();
    void        OpenArchive();
    void        OpenEntry();
    void        ReadEntry();
    void        FinishArchive();
    void        Fail(const String & Reason);
    void        CloseArchive();
    void        UpdateTerminalState();
    bool        FileInUse(const String & FileName) const;
    void        LockStatus();
    void        UnlockStatus();
#ifdef ARDUINO_ARCH_ESP32
    SemaphoreHandle_t StatusMutex = nullptr;
#endif
    // Status requests run in the asynchronous web-server task on ESP32.
    struct StatusGuard
    {
        UnzipFiles & Owner;
        explicit StatusGuard(UnzipFiles & Value) : Owner(Value) { Owner.LockStatus(); }
        ~StatusGuard() { Owner.UnlockStatus(); }
    };

protected:

}; // UnzipFiles

extern UnzipFiles gUnzipFiles;

#endif // def SUPPORT_UNZIP
