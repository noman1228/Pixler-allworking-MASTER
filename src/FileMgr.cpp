/*
* FileMgr.cpp - Output Management class
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

#include "ESPixelStick.h"
#include <Int64String.h>
#include <TimeLib.h>

#include "FileMgr.hpp"
#include "network/NetworkMgr.hpp"
#include "output/OutputMgr.hpp"
#include "input/InputMgr.hpp"
#include "service/FPPDiscovery.h"
#include "UnzipFiles.hpp"

SdFs sd;
const int8_t DISABLE_CS_PIN = -1;

#if HAS_SDIO_CLASS
#define SD_CONFIG SdioConfig(FIFO_SDIO)
#elif ENABLE_DEDICATED_SPI
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, DEDICATED_SPI, SD_SCK_MHZ(16))
#else  // HAS_SDIO_CLASS
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(16))
#endif  // HAS_SDIO_CLASS

#ifdef SIMULATE_SD
char XlateFileMode[3] = { CN_r[0], CN_w[0], CN_w[0] };
#else
oflag_t XlateFileMode[3] = { O_READ , O_WRITE | O_CREAT, O_WRITE | O_APPEND };
#endif // def SIMULATE_SD

// SdFat can stop openNext() at a malformed LFN without setting a read error.
// A later file can still be opened by name; do not hide it from every SD scan.
bool c_FileMgr::OpenNextSdEntry(FsFile & Entry, FsFile & Directory)
{
    while (true)
    {
        FeedWDT();
        const uint64_t start = Directory.curPosition();
        if (Entry.openNext(&Directory, O_READ))
        {
            return true;
        }
        if (Directory.getError())
        {
            logcon(F("ERROR: SD directory read failed"));
            return false;
        }
        const uint64_t next = Directory.curPosition();
        if (next <= start || next < 32)
        {
            return false;
        }
        // openNext consumes the 32-byte directory record even on an ordinary
        // end marker. Inspect that marker before deciding whether to retry.
        uint8_t entryType = 0;
        if (!Directory.seekSet(next - 32) || Directory.read(&entryType, 1) != 1 ||
            !Directory.seekSet(next))
        {
            logcon(F("ERROR: Could not inspect SD directory entry"));
            return false;
        }
        if (entryType == 0)
        {
            return false;
        }
        logcon(String(F("WARNING: Skipping invalid SD directory entry at offset ")) +
               int64String(next - 32));
    }
}

#ifdef SUPPORT_FTP
#include <SimpleFTPServer.h>
FtpServer   ftpSrv;
static bool FtpUploadInProgress = false;
static String FtpUploadName;

static void FinishFtpUpload()
{
    const bool wasUploading = FtpUploadInProgress;
    FtpUploadInProgress = false;
#ifdef SUPPORT_UNZIP
    String completedName = FtpUploadName;
    completedName.toLowerCase();
    if (wasUploading && (completedName.endsWith(".zip") || completedName.endsWith(".xlz")))
    {
        gUnzipFiles.Run();
    }
#else
    (void)wasUploading;
#endif
    FtpUploadName.clear();
}

//-----------------------------------------------------------------------------
void ftp_callback(FtpOperation ftpOperation, unsigned int freeSpace, unsigned int totalSpace)
{
    FeedWDT();
    switch (ftpOperation)
    {
        case FTP_CONNECT:
        {
            InputMgr.SetOperationalState(false);
            LOG_PORT.println(F("FTP: Connected!"));
            break;
        }

        case FTP_DISCONNECT:
        {
            FtpUploadInProgress = false;
            FtpUploadName.clear();
            InputMgr.SetOperationalState(true);
            LOG_PORT.println(F("FTP: Disconnected!"));
            break;
        }

        case FTP_FREE_SPACE_CHANGE:
        {
            // This also covers empty/very short transfers for which the FTP
            // library omits its transfer-stop callback.
            FinishFtpUpload();
            FileMgr.BuildFseqList(false);
            LOG_PORT.printf("FTP: Free space change, free %u of %u! Rebuilding FSEQ file list\n", freeSpace, totalSpace);
            break;
        }

        default:
        {
            LOG_PORT.println(F("FTP: unknown callback!"));
            break;
        }
    }
};

//-----------------------------------------------------------------------------
void ftp_transferCallback(FtpTransferOperation ftpOperation, const char* name, unsigned int transferredSize)
{
    FeedWDT();
    switch (ftpOperation)
    {
        case FTP_UPLOAD_START:
        {
            FtpUploadInProgress = true;
            FtpUploadName = name;
            LOG_PORT.println(String(F("FTP: Start Uploading '")) + name + "'");
            // size_t free = heap_caps_get_largest_free_block(0x1800);
            // LOG_PORT.printf("heap: 0x%x\n", free);
            break;
        }

        case FTP_UPLOAD:
        {
            // LOG_PORT.printf("FTP: Upload of file %s byte %u\n", name, transferredSize);
            // size_t free = heap_caps_get_largest_free_block(0x1800);
            // LOG_PORT.printf("heap: 0x%x\n", free);
            break;
        }

        case FTP_TRANSFER_STOP:
        {
            // The library already closed the file; use its saved start name.
            FinishFtpUpload();
            LOG_PORT.println(String(F("FTP: Done Uploading '")) + name + "'");
            // size_t free = heap_caps_get_largest_free_block(0x1800);
            // LOG_PORT.printf("heap: 0x%x\n", free);
            break;
        }

        case FTP_TRANSFER_ERROR:
        {
            FtpUploadInProgress = false;
            FtpUploadName.clear();
            LOG_PORT.println(String(F("FTP: Error Uploading '")) + name + "'");
            break;
        }

        default:
        {
            LOG_PORT.println(F("FTP: Unknown Transfer Callback!"));
            break;
        }
    }

  /* FTP_UPLOAD_START = 0,
   * FTP_UPLOAD = 1,
   *
   * FTP_DOWNLOAD_START = 2,
   * FTP_DOWNLOAD = 3,
   *
   * FTP_TRANSFER_STOP = 4,
   * FTP_DOWNLOAD_STOP = 4,
   * FTP_UPLOAD_STOP = 4,
   *
   * FTP_TRANSFER_ERROR = 5,
   * FTP_DOWNLOAD_ERROR = 5,
   * FTP_UPLOAD_ERROR = 5
   */
};
#endif // def SUPPORT_FTP

//-----------------------------------------------------------------------------
///< Start up the driver and put it into a safe mode
c_FileMgr::c_FileMgr ()
{
#ifdef ARDUINO_ARCH_ESP32
    SdAccessSemaphore = xSemaphoreCreateBinary();
    UnLockSd();
#endif // def ARDUINO_ARCH_ESP32
    fsUploadFileName.reserve(256);
} // c_FileMgr

//-----------------------------------------------------------------------------
///< deallocate any resources and put the output channels into a safe state
c_FileMgr::~c_FileMgr ()
{
    // DEBUG_START;

    // DEBUG_END;

} // ~c_FileMgr

//-----------------------------------------------------------------------------
///< Start the module
void c_FileMgr::Begin ()
{
    // DEBUG_START;

    do // once
    {
        InitSdFileList ();

        if (!LittleFS.begin ())
        {
            String msg = String(CN_stars) + F (" Flash file system did not initialize correctly ") + CN_stars;
            RequestReboot(msg, 1000, true);
            break;
        }

#ifdef ARDUINO_ARCH_ESP32
            logcon (String (F ("Flash file system initialized. Used = ")) + String (LittleFS.usedBytes ()) + String (F (" out of ")) + String (LittleFS.totalBytes()) );
#else
            logcon (String (F ("Flash file system initialized.")));
#endif // def ARDUINO_ARCH_ESP32

            listDir (LittleFS, String ("/"), 3);

        StartSdCard();

    } while (false);

    // DEBUG_END;
} // begin

//-----------------------------------------------------------------------------
void c_FileMgr::StartSdCard ()
{
    // DEBUG_START;

    SetSpiIoPins ();
    #ifdef SUPPORT_UNZIP
    // DEBUG_V(String("FoundZipFile: ") + String(FoundZipFile))
    if(FoundZipFile)
    {
        // Discover archives now; extraction waits for the network in Poll().
        FeedWDT();
        gUnzipFiles.Run();
    }
    #endif // def SUPPORT_UNZIP

    // DEBUG_END;
} // StartSdCard

//-----------------------------------------------------------------------------
void c_FileMgr::NetworkStateChanged (bool NewState)
{
    // DEBUG_START;
#ifdef SUPPORT_FTP
    // DEBUG_V("Disable FTP");
    ftpSrv.end();

    // DEBUG_V(String("       NewState: ") + String(NewState));
    // DEBUG_V(String("SdCardInstalled: ") + String(SdCardInstalled));
    // DEBUG_V(String("     FtpEnabled: ") + String(FtpEnabled));
    if(NewState && SdCardInstalled && FtpEnabled)
    {
        logcon("Starting FTP server.");
        ftpSrv.setCallback(ftp_callback);
        ftpSrv.setTransferCallback(ftp_transferCallback);
        ftpSrv.begin(FtpUserName, FtpPassword, WelcomeString);
    }
    else
    {
        // DEBUG_V("Not starting FTP service");
    }
#endif // def SUPPORT_FTP

    // DEBUG_END;
} // NetworkStateChanged

//-----------------------------------------------------------------------------
void c_FileMgr::Poll()
 {
    // xDEBUG_START;
#ifdef SUPPORT_FTP
    if(FtpEnabled)
    {
        FeedWDT();
        ftpSrv.handleFTP();
    }
#endif // def SUPPORT_FTP
    // xDEBUG_END;
 } // Poll

//-----------------------------------------------------------------------------
bool c_FileMgr::SetConfig (JsonObject & json)
{
    // DEBUG_START;

    bool ConfigChanged = false;
    bool SpiConfigChanged = false;
    JsonObject JsonDeviceConfig = json[(char*)CN_device].as<JsonObject>();
    if (JsonDeviceConfig)
    {
        // PrettyPrint (JsonDeviceConfig, "c_FileMgr");

        // DEBUG_V("miso_pin: " + String(miso_pin));
        // DEBUG_V("mosi_pin: " + String(mosi_pin));
        // DEBUG_V(" clk_pin: " + String(clk_pin));
        // DEBUG_V("  cs_pin: " + String(cs_pin));
        // DEBUG_V("   Speed: " + String(MaxSdSpeed));

        SpiConfigChanged |= setFromJSON (miso_pin,   JsonDeviceConfig, CN_miso_pin);
        SpiConfigChanged |= setFromJSON (mosi_pin,   JsonDeviceConfig, CN_mosi_pin);
        SpiConfigChanged |= setFromJSON (clk_pin,    JsonDeviceConfig, CN_clock_pin);
        SpiConfigChanged |= setFromJSON (cs_pin,     JsonDeviceConfig, CN_cs_pin);
        SpiConfigChanged |= setFromJSON (MaxSdSpeed, JsonDeviceConfig, CN_sdspeed);

        ConfigChanged |= setFromJSON (FtpUserName, JsonDeviceConfig, CN_user);
        ConfigChanged |= setFromJSON (FtpPassword, JsonDeviceConfig, CN_password);
        ConfigChanged |= setFromJSON (FtpEnabled,  JsonDeviceConfig, CN_enabled);

        // DEBUG_V("miso_pin: " + String(miso_pin));
        // DEBUG_V("mosi_pin: " + String(mosi_pin));
        // DEBUG_V(" clk_pin: " + String(clk_pin));
        // DEBUG_V("  cs_pin: " + String(cs_pin));
        // DEBUG_V("   Speed: " + String(MaxSdSpeed));

        // DEBUG_V("ConfigChanged: " + String(ConfigChanged));
    }
    else
    {
        logcon (F ("No File Manager settings found."));
    }

    // DEBUG_V (String ("ConfigChanged: ") + String (ConfigChanged));
    // DEBUG_V (String ("SpiConfigChanged: ") + String (SpiConfigChanged));

    if(IsBooting)
    {
        // DEBUG_V("We are booting");
        StartSdCard ();
        SpiConfigChanged = false;
    }

    else if (SpiConfigChanged)
    {
        String msg = "SD Card gpio change requires reboot";
        RequestReboot(msg,1000);
    }

    if (ConfigChanged)
    {
        NetworkStateChanged(NetworkMgr.IsConnected());
    }

    // DEBUG_END;

    return ConfigChanged || SpiConfigChanged;

} // SetConfig

//-----------------------------------------------------------------------------
void c_FileMgr::GetConfig (JsonObject& json)
{
    // DEBUG_START;

    JsonWrite(json, CN_miso_pin,  miso_pin);
    JsonWrite(json, CN_mosi_pin,  mosi_pin);
    JsonWrite(json, CN_clock_pin, clk_pin);
    JsonWrite(json, CN_cs_pin,    cs_pin);
    JsonWrite(json, CN_sdspeed,   MaxSdSpeed);

    JsonWrite(json, CN_user,      FtpUserName);
    JsonWrite(json, CN_password,  FtpPassword);
    JsonWrite(json, CN_enabled,   FtpEnabled);

    // DEBUG_END;

} // GetConfig

//-----------------------------------------------------------------------------
void c_FileMgr::GetStatus (JsonObject& json)
{
    // DEBUG_START;

#ifdef ARDUINO_ARCH_ESP32
    json[F ("size")] = LittleFS.totalBytes ();
    json[F ("used")] = LittleFS.usedBytes ();
#endif // def ARDUINO_ARCH_ESP32

    // DEBUG_END;

} // GetConfig

//------------------------------------------------------------------------------
// Call back for file timestamps.  Only called for file create and sync().
void dateTime(uint16_t* date, uint16_t* time, uint8_t* ms10)
{
    // LOG_PORT.println("DateTime Start");

    tmElements_t tm;
    breakTime(now(), tm);

    // LOG_PORT.println(String("time_t:") + String(now()));
    // LOG_PORT.println(String("Year Raw:") + String(tm.Year));
    // LOG_PORT.println(String("Year Adj:") + String(tm.Year + 1970));
    // LOG_PORT.println(String("Month:") + String(tm.Month));
    // LOG_PORT.println(String("Day:") + String(tm.Day));

    // Return date using FS_DATE macro to format fields.
    *date = FS_DATE(tm.Year+1970, tm.Month, tm.Day);
    // LOG_PORT.println(String("date:") + String(*date));

    // Return time using FS_TIME macro to format fields.
    *time = FS_TIME(tm.Hour, tm.Minute, tm.Second);
    // LOG_PORT.println(String("time:") + String(*time));

    // Return low time bits in units of 10 ms, 0 <= ms10 <= 199.
    *ms10 = 0;
    // LOG_PORT.println("DateTime End");
}

//-----------------------------------------------------------------------------
void c_FileMgr::SetSpiIoPins ()
{
    // DEBUG_START;

#if defined ARDUINO_ARCH_ESP8266
    ESP.wdtDisable();
#endif // def ARDUINO_ARCH_ESP8266

#if defined (SIMULATE_SD)
    if(!ESP_SDFS.exists(CN_slashsd))
    {
        ESP_SDFS.mkdir(CN_slashsd);
    }

    SdCardInstalled = true;
    SetSdSpeed();
    BuildFseqList(true);

#elif defined (SUPPORT_SD) || defined(SUPPORT_SD_MMC)
    if (SdCardInstalled)
    {
        // DEBUG_V("Terminate current SD session");
        ESP_SD.end ();
    }

    FsDateTime::setCallback(dateTime);
#ifdef ARDUINO_ARCH_ESP32
    // DEBUG_V();
    try
#endif // def ARDUINO_ARCH_ESP32
    {
#ifdef SIMULATE_SD
#elif defined (SUPPORT_SD_MMC)
        // DEBUG_V (String ("  Data 0: ") + String (SD_CARD_DATA_0));
        // DEBUG_V (String ("  Data 1: ") + String (SD_CARD_DATA_1));
        // DEBUG_V (String ("  Data 2: ") + String (SD_CARD_DATA_2));
        // DEBUG_V (String ("  Data 3: ") + String (SD_CARD_DATA_3));

        pinMode(SD_CARD_DATA_0, PULLUP);
        pinMode(SD_CARD_DATA_1, PULLUP);
        pinMode(SD_CARD_DATA_2, PULLUP);
        pinMode(SD_CARD_DATA_3, PULLUP);

        if(!ESP_SD.begin())
#else // ifndef SUPPORT_SD_MMC
#   ifdef ARDUINO_ARCH_ESP32
        // DEBUG_V (String ("miso_pin: ") + String (miso_pin));
        // DEBUG_V (String ("mosi_pin: ") + String (mosi_pin));
        // DEBUG_V (String (" clk_pin: ") + String (clk_pin));
        // DEBUG_V (String ("  cs_pin: ") + String (cs_pin));

        // DEBUG_V();
        SPI.end ();
        // DEBUG_V();
        pinMode(cs_pin, OUTPUT);
        // DEBUG_V();
#       ifdef USE_MISO_PULLUP
        // DEBUG_V("USE_MISO_PULLUP");
        // on some hardware MISO is missing a required pull-up resistor, use internal pull-up.
        ResetGpio(gpio_num_t(mosi_pin));
        pinMode(miso_pin, INPUT_PULLUP);
#       else
        // DEBUG_V();
        pinMode(miso_pin, INPUT);
        // DEBUG_V();
#       endif // def USE_MISO_PULLUP
        // DEBUG_V();
        SPI.begin (clk_pin, miso_pin, mosi_pin, cs_pin); // uses HSPI by default
#   else // ESP8266
        SPI.end ();
        SPI.begin ();
#   endif // ! ARDUINO_ARCH_ESP32
        // DEBUG_V();
        if (!ESP_SD.begin (SdSpiConfig(cs_pin, SHARED_SPI, SD_SCK_MHZ(16))))
#endif // !def SUPPORT_SD_MMC
        {
            // DEBUG_V();
            logcon(String(F("No SD card installed")));
            SdCardInstalled = false;
            // DEBUG_V();
            if(nullptr == ESP_SD.card())
            {
                logcon(F("SD 'Card' setup failed."));
            }
            else if (ESP_SD.card()->errorCode())
            {
                logcon(String(F("SD initialization failed - code: ")) + String(ESP_SD.card()->errorCode()));
                // DEBUG_V(String(F("SD initialization failed - data: ")) + String(ESP_SD.card()->errorData()));
                printSdErrorText(&Serial, ESP_SD.card()->errorCode()); LOG_PORT.println("");
            }
            else if (ESP_SD.vol()->fatType() == 0)
            {
                logcon(F("SD Can't find a valid FAT16/FAT32 partition."));
            }
            else
            {
                logcon(F("SD Can't determine error type"));
            }
        }
        else
        {
            // DEBUG_V("SdCardInstalled = true");
            SdCardInstalled = true;
            // DEBUG_V();
            SetSdSpeed ();

            // DEBUG_V(String("SdCardSize: ") + int64String(SdCardSize));

            DescribeSdCardToUser ();
            // DEBUG_V();
        }

        // DEBUG_V();
        BuildFseqList(true);
        // DEBUG_V();
    }
#ifdef ARDUINO_ARCH_ESP32
    catch (const std::exception &e)
    {
        logcon (String (F ("ERROR: Could not init the SD Card: "))+ e.what());
        SdCardInstalled = false;
    }
#endif // def ARDUINO_ARCH_ESP32

    // SdFile::dateTimeCallback(dateTime);

#else // no sd defined
    SdCardInstalled = false;
#endif // defined (SUPPORT_SD) || defined(SUPPORT_SD_MMC)

#ifdef ARDUINO_ARCH_ESP8266
    ESP.wdtEnable(uint32_t(0));
#endif // def ARDUINO_ARCH_ESP8266
    // DEBUG_END;

} // SetSpiIoPins

//-----------------------------------------------------------------------------
void c_FileMgr::SetSdSpeed ()
{
    // DEBUG_START;
#ifdef SIMULATE_SD
    SdCardSize = ESP_SD.totalBytes();
#elif defined (SUPPORT_SD) || defined(SUPPORT_SD_MMC)
    if (SdCardInstalled)
    {
        // DEBUG_V();
        csd_t csd;
        uint8_t tran_speed = 0;

        CSD MyCsd;
        // DEBUG_V();
        ESP_SD.card()->readCSD(&csd);
        memcpy (&MyCsd, &csd.csd[0], sizeof(csd.csd));
        // DEBUG_V(String("TRAN Speed: 0x") + String(MyCsd.Decode_0.tran_speed,HEX));
        tran_speed = MyCsd.Decode_0.tran_speed;
        uint32_t FinalTranSpeedMHz = MaxSdSpeed;

        switch(tran_speed)
        {
            case 0x32:
            {
                FinalTranSpeedMHz = 25;
                break;
            }
            case 0x5A:
            {
                FinalTranSpeedMHz = 50;
                break;
            }
            case 0x0B:
            {
                FinalTranSpeedMHz = 100;
                break;
            }
            case 0x2B:
            {
                FinalTranSpeedMHz = 200;
                break;
            }
            default:
            {
                FinalTranSpeedMHz = 25;
            }
        }
        // DEBUG_V();
        FinalTranSpeedMHz = min(FinalTranSpeedMHz, MaxSdSpeed);
        SPI.setFrequency(FinalTranSpeedMHz * 1024 * 1024);
        logcon(String("Set SD speed to ") + String(FinalTranSpeedMHz) + "Mhz");

        SdCardSize =1024 *1024 * 0.000512 * csd.capacity();
        // DEBUG_V(String("SdCardSize: ") + int64String(SdCardSize));
    }
#endif // defined (SUPPORT_SD) || defined(SUPPORT_SD_MMC)

    // DEBUG_END;

} // SetSdSpeed

//-----------------------------------------------------------------------------
/*
    On occasion, the SD card is left in an unknown state that causes the SD card
    to appear to not be installed. A power cycle fixes the issue. Clocking the
    bus also causes any incomplete transaction to get cleared.
*/
void c_FileMgr::ResetSdCard()
{
    // DEBUG_START;
#ifndef SIMULATE_SD
    // send 0xff bytes until we get 0xff back
    byte ResetValue = 0x00;
    digitalWrite(cs_pin, LOW);
    SPI.beginTransaction(SPISettings());
    // DEBUG_V();
    uint32_t retry = 2000;
    while((0xff != ResetValue) && (--retry))
    {
        delay(1);
        ResetValue = SPI.transfer(0xff);
    }
    SPI.endTransaction();
    digitalWrite(cs_pin, HIGH);
#endif // ndef SIMULATE_SD

    // DEBUG_END;
} // ResetSdCard

//-----------------------------------------------------------------------------
void c_FileMgr::DeleteFlashFile (String FileName)
{
    // DEBUG_START;

    ConnrectFilename(FileName);

    if(LittleFS.exists(FileName)) { LittleFS.remove (FileName); }

#ifdef SIMULATE_SD
    if(FileName.startsWith(CN_slashsdslash))
    {
        BuildFseqList(false);
    }
#endif

    // DEBUG_END;

} // DeleteConfigFile

//-----------------------------------------------------------------------------
bool c_FileMgr::RenameFlashFile (String OldName, String NewName)
{
     // DEBUG_START;

    ConnrectFilename(OldName);
    ConnrectFilename(NewName);

     // DEBUG_V(String("OldName: ") + OldName);
     // DEBUG_V(String("NewName: ") + NewName);

    // LittleFS replaces an existing file in the rename operation. Keep the
    // previous config available until this succeeds, including on failure.
    if(!LittleFS.rename(OldName, NewName))
    {
        logcon(String(CN_stars) + F("Could not rename '") + OldName + F("' to '") + NewName + F("'") + CN_stars);
        return false;
    }

#ifdef SIMULATE_SD
    if(OldName.startsWith(CN_slashsdslash) || NewName.startsWith(CN_slashsdslash))
    {
        BuildFseqList(false);
    }
#endif

     // DEBUG_END;
    return true;
} // RenameFlashFile

//-----------------------------------------------------------------------------
void c_FileMgr::listDir (fs::FS& fs, String dirname, uint8_t levels)
{
    // DEBUG_START;
    do // once
    {
        logcon (String (F ("Listing directory: ")) + dirname);

        File root = fs.open (dirname, CN_r);
        if (!root)
        {
            logcon (String (CN_stars) + F ("failed to open directory: ") + dirname + CN_stars);
            break;
        }

        if (!root.isDirectory ())
        {
            logcon (String (F ("Is not a directory: ")) + dirname);
            break;
        }

        File MyFile = root.openNextFile ();

        while (MyFile)
        {
            if (MyFile.isDirectory ())
            {
                if (levels)
                {
                    listDir (fs, dirname + "/" + MyFile.name (), levels - 1);
                }
            }
            else
            {
                logcon ("'" + String (MyFile.name ()) + "': \t'" + String (MyFile.size ()) + "'");
            }
            MyFile = root.openNextFile ();
        }

    } while (false);

} // listDir

//-----------------------------------------------------------------------------
bool c_FileMgr::LoadFlashFile (const String& FileName, DeserializationHandler Handler)
{
    // DEBUG_START;

    bool retval = false;

    do // once
    {
        String CfgFileMessagePrefix = String (CN_Configuration_File_colon) + "'" + FileName + "' ";

        // DEBUG_V ("allocate the JSON Doc");
/*
        String RawFileData;
        if (false == ReadConfigFile (FileName, RawFileData))
        {
            logcon (String(CN_stars) + CfgFileMessagePrefix + F ("Could not read file.") + CN_stars);
            break;
        }
        logcon(RawFileData);
*/
        // DEBUG_V();
        fs::File file = LittleFS.open (FileName.c_str (), CN_r);
        // DEBUG_V();
        if (!file)
        {
            if (!IsBooting)
            {
                logcon (String (CN_stars) + CfgFileMessagePrefix + String (F (" Could not open file for reading ")) + CN_stars);
            }
            break;
        }

        JsonDocument jsonDoc;
        jsonDoc.to<JsonObject>();

        // DEBUG_V ("Convert File to JSON document");
        DeserializationError error = deserializeJson (jsonDoc, file);
        file.close ();

        // DEBUG_V ("Error Check");
        if (error)
        {
            // logcon (CN_Heap_colon + String (ESP.getMaxFreeBlockSize ()));
            logcon (String(CN_stars) + CfgFileMessagePrefix + String (F ("Deserialzation Error. Error code = ")) + error.c_str () + CN_stars);
            // logcon (CN_plussigns + RawFileData + CN_minussigns);
	        // DEBUG_V (String ("                heap: ") + String (ESP.getFreeHeap ()));
    	    //  xDEBUG_V (String (" getMaxFreeBlockSize: ") + String (ESP.getMaxFreeBlockSize ()));
        	// xDEBUG_V (String ("           file.size: ") + String (file.size ()));
            break;
        }

        // DEBUG_V ();
        logcon (CfgFileMessagePrefix + String (F ("loaded.")));

        // DEBUG_V ();
        Handler (jsonDoc);

        // DEBUG_V();
        retval = true;

    } while (false);

    // DEBUG_END;
    return retval;

} // LoadFlashFile

//-----------------------------------------------------------------------------
bool c_FileMgr::SaveFlashFile (const String& FileName, String& FileData)
{
    // DEBUG_START;

    bool Response = SaveFlashFile (FileName, FileData.c_str ());

    // DEBUG_END;
    return Response;
} // SaveConfigFile

//-----------------------------------------------------------------------------
bool c_FileMgr::SaveFlashFile (const String& FileName, const char * FileData)
{
    // DEBUG_START;

    bool Response = false;
    String CfgFileMessagePrefix = String (CN_Configuration_File_colon) + "'" + FileName + "' ";
    // DEBUG_V (FileData);

    fs::File file = LittleFS.open (FileName.c_str (), CN_w);
    if (!file)
    {
        logcon (String (CN_stars) + CfgFileMessagePrefix + String (F ("Could not open file for writing..")) + CN_stars);
    }
    else
    {
        file.seek (0, SeekSet);
        file.print (FileData);
        file.close ();

        file = LittleFS.open (FileName.c_str (), CN_r);
        logcon (CfgFileMessagePrefix + String (F ("saved ")) + String (file.size ()) + F (" bytes."));
        file.close ();

        Response = true;
    }

    // DEBUG_END;
    return Response;
} // SaveConfigFile

//-----------------------------------------------------------------------------
bool c_FileMgr::SaveFlashFile(const String &FileName, JsonDocument &FileData)
{
    // DEBUG_START;
    bool Response = false;

    String CfgFileMessagePrefix = String(CN_Configuration_File_colon) + "'" + FileName + "' ";
    // DEBUG_V(String("CfgFileMessagePrefix: ") + CfgFileMessagePrefix);

    // serializeJson(FileData, LOG_PORT);
    // DEBUG_V("");

    // delay(100);
    // DEBUG_V("");

    fs::File file = LittleFS.open(FileName.c_str(), CN_w);
    // DEBUG_V("");

    if (!file)
    {
        logcon(String(CN_stars) + CfgFileMessagePrefix + String(F("Could not open file for writing..")) + CN_stars);
    }
    else
    {
        // DEBUG_V("");
        file.seek(0, SeekSet);
        // DEBUG_V("");

        size_t NumBytesSaved = serializeJson(FileData, file);
        // DEBUG_V("");

        file.close();

        logcon(CfgFileMessagePrefix + String(F("saved ")) + String(NumBytesSaved) + F(" bytes."));

        Response = true;
    }

    // DEBUG_END;

    return Response;
} // SaveConfigFile

//-----------------------------------------------------------------------------
bool c_FileMgr::SaveFlashFile(const String & FileName, uint32_t index, uint8_t *data, uint32_t len, bool final)
{
    // DEBUG_START;
    bool Response = false;

    String CfgFileMessagePrefix = String(CN_Configuration_File_colon) + "'" + FileName + "' ";
    // DEBUG_V(String("CfgFileMessagePrefix: ") + CfgFileMessagePrefix);
    // DEBUG_V(String("            FileName: ") + FileName);
    // DEBUG_V(String("               index: ") + String(index));
    // DEBUG_V(String("                 len: ") + String(len));
    // DEBUG_V(String("               final: ") + String(final));

    fs::File file;
    String FilePath = FileName;
    ConnrectFilename(FilePath);

    if(0 == index)
    {
        file = LittleFS.open(FilePath.c_str(), CN_w);
    }
    else
    {
        file = LittleFS.open(FilePath.c_str(), "a");
    }
    // DEBUG_V("");

    if (!file)
    {
        logcon(String(CN_stars) + CfgFileMessagePrefix + String(F("Could not open file for writing..")) + CN_stars);
    }
    else
    {
        // DEBUG_V("");
        // Refuse missing, repeated, or out-of-order chunks rather than
        // committing a partial config over the previous good file.
        if (file.size() != index || !file.seek(index, SeekMode::SeekSet))
        {
            file.close();
            return false;
        }
        // DEBUG_V("");

        size_t NumBytesSaved = file.write(data, size_t(len));
        // DEBUG_V("");

        file.close();

        logcon(CfgFileMessagePrefix + String(F("saved ")) + String(NumBytesSaved) + F(" bytes."));

        Response = (NumBytesSaved == len);
    }

    // DEBUG_END;

    return Response;
} // SaveConfigFile

//-----------------------------------------------------------------------------
bool c_FileMgr::ReadFlashFile (const String& FileName, String& FileData)
{
    // DEBUG_START;

    bool GotFileData = false;
    String CfgFileMessagePrefix = String (CN_Configuration_File_colon) + "'" + FileName + "' ";

    // DEBUG_V (String("File '") + FileName + "' is being opened.");
    fs::File file = LittleFS.open (FileName.c_str (), CN_r);
    if (file)
    {
        // Suppress this for now, may add it back later
        //logcon (CfgFileMessagePrefix + String (F ("reading ")) + String (file.size()) + F (" bytes."));

        // DEBUG_V (String("File '") + FileName + "' is open.");
        file.seek (0, SeekSet);
        FileData = file.readString ();
        file.close ();
        GotFileData = true;

        // DEBUG_V (FileData);
    }
    else
    {
        logcon (String (CN_stars) + CN_Configuration_File_colon + "'" + FileName + F ("' not found.") + CN_stars);
    }

    // DEBUG_END;
    return GotFileData;
} // ReadConfigFile

//-----------------------------------------------------------------------------
bool c_FileMgr::ReadFlashFile (const String& FileName, JsonDocument & FileData)
{
    // DEBUG_START;
    bool GotFileData = false;

    do // once
    {
        String RawFileData;
        if (false == ReadFlashFile (FileName, RawFileData))
        {
            // DEBUG_V ("Failed to read file");
            break;
        }

        // did we actually get any data
        if (0 == RawFileData.length ())
        {
            // DEBUG_V ("File is empty");
            // nope, no data
            GotFileData = false;
            break;
        }

        // DEBUG_V ("Convert File to JSON document");
        DeserializationError error = deserializeJson (FileData, (const String)RawFileData);

        // DEBUG_V ("Error Check");
        if (error)
        {
            String CfgFileMessagePrefix = String (CN_Configuration_File_colon) + "'" + FileName + "' ";
            logcon (CN_Heap_colon + String (ESP.getFreeHeap ()));
            logcon (CfgFileMessagePrefix + String (F ("Deserialzation Error. Error code = ")) + error.c_str ());
            logcon (CN_plussigns + RawFileData + CN_minussigns);
            break;
        }

        // DEBUG_V ("Got config data");
        GotFileData = true;

    } while (false);

    // DEBUG_END;
    return GotFileData;

} // ReadConfigFile

//-----------------------------------------------------------------------------
bool c_FileMgr::ReadFlashFile (const String & FileName, byte * FileData, size_t maxlen)
{
    // DEBUG_START;
    bool GotFileData = false;

    do // once
    {
        // DEBUG_V (String("File '") + FileName + "' is being opened.");
        fs::File file = LittleFS.open (FileName.c_str (), CN_r);
        if (!file)
        {
            logcon (String (CN_stars) + CN_Configuration_File_colon + "'" + FileName + F ("' not found.") + CN_stars);
            break;
        }

        if (file.size() >= maxlen)
        {
            logcon (String (CN_stars) + CN_Configuration_File_colon + "'" + FileName + F ("' too large for buffer. ") + CN_stars);
            file.close ();
            break;
        }

        // Suppress this for now, may add it back later
        // Done at interrupt level. Cant use Strings
        // LOG_PORT.print   (FileName);
        // LOG_PORT.print   (" reading ");
        // LOG_PORT.print   (file.size ());
        // LOG_PORT.println (" bytes.");

        // DEBUG_V (String("File '") + FileName + "' is open.");
        file.seek (0, SeekSet);
        file.read (FileData, file.size());
        file.close ();

        GotFileData = true;

        // DEBUG_V (FileData);

    } while (false);

    // DEBUG_END;
    return GotFileData;

} // ReadConfigFile

//-----------------------------------------------------------------------------
bool c_FileMgr::FlashFileExists (const String & FileName)
{
    return LittleFS.exists (FileName.c_str ());

} // FlashFileExists

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void c_FileMgr::InitSdFileList ()
{
    // DEBUG_START;

    int index = 0;
    for (auto& currentFileListEntry : FileList)
    {
        currentFileListEntry.handle  = INVALID_FILE_HANDLE;
        currentFileListEntry.entryId = index++;
        currentFileListEntry.Filename.reserve(256);
    }

    // DEBUG_END;

} // InitFileList

//-----------------------------------------------------------------------------
int c_FileMgr::FileListFindSdFileHandle (FileId HandleToFind)
{
    // DEBUG_START;

    int response = -1;
    // DEBUG_V (String ("HandleToFind: ") + String (HandleToFind));

    if(INVALID_FILE_HANDLE != HandleToFind)
    {
        for (auto & currentFileListEntry : FileList)
        {
            // DEBUG_V (String ("currentFileListEntry.handle: ")  + String (currentFileListEntry.handle));
            // DEBUG_V (String ("currentFileListEntry.entryId: ") + String (currentFileListEntry.entryId));

            if (currentFileListEntry.handle == HandleToFind)
            {
                response = currentFileListEntry.entryId;
                break;
            }
        }
    }

    // DEBUG_END;

    return response;
} // FileListFindSdFileHandle

//-----------------------------------------------------------------------------
c_FileMgr::FileId c_FileMgr::CreateSdFileHandle ()
{
    // DEBUG_START;

    FileId response = INVALID_FILE_HANDLE;
    FileId FileHandle = millis ();

    // create a unique handle
    while (-1 != FileListFindSdFileHandle (FileHandle))
    {
        ++FileHandle;
    }

    // find an empty slot
    for (auto & currentFileListEntry : FileList)
    {
        if (currentFileListEntry.handle == INVALID_FILE_HANDLE)
        {
            currentFileListEntry.handle = FileHandle;
            response = FileHandle;
            // DEBUG_V(String("handle: ") + currentFileListEntry.handle);
            break;
        }
    }

    if (INVALID_FILE_HANDLE == response)
    {
        logcon (String (CN_stars) + F (" Could not allocate another file handle ") + CN_stars);
    }
    // DEBUG_V (String ("FileHandle: ") + String (FileHandle));

    // DEBUG_END;

    return response;
} // CreateFileHandle

//-----------------------------------------------------------------------------
void c_FileMgr::DeleteSdFile (const String & FileName)
{
    // DEBUG_START;
#ifdef SIMULATE_SD
    String NewFilename = String(CN_slashsdslash) + FileName;
    DeleteFlashFile(NewFilename);
#else
    LockSd();
    bool FileExists = ESP_SD.exists (FileName);
    UnLockSd();
    if (FileExists)
    {
        // DEBUG_V (String ("Deleting '") + FileName + "'");
        LockSd();
        ESP_SD.remove (FileName);
        UnLockSd();
    }
#endif // def SIMULATE_SD
    BuildFseqList(false);

    // DEBUG_END;

} // DeleteSdFile

//-----------------------------------------------------------------------------
void c_FileMgr::DescribeSdCardToUser ()
{
    // DEBUG_START;

    logcon (String (F ("SD Card Size: ")) + int64String (SdCardSize) + "Bytes");
/*
    // DEBUG_V("Open Root");
    FsFile root;
    ESP_SD.chdir();
    root.open("/", O_READ);
    printDirectory (root, 0);
    root.close();
    // DEBUG_V("Close Root");
*/
    // DEBUG_END;

} // DescribeSdCardToUser

//-----------------------------------------------------------------------------
void c_FileMgr::GetListOfSdFiles (std::vector<String> & Response)
{
    // DEBUG_START;

    char entryName[256];

    Response.clear();
    LockSd();
    do // once
    {
        if (false == SdCardIsInstalled ())
        {
            // DEBUG_V("No SD Card installed");
            break;
        }

#ifdef SIMULATE_SD

        File root = ESP_SD.open (CN_slashsd, CN_r);
        if (!root)
        {
            logcon (String (CN_stars) + F ("failed to open directory: ") + CN_slashsd + CN_stars);
            break;
        }

        if (!root.isDirectory ())
        {
            logcon (String (F ("Is not a directory: ")) + CN_slashsd);
            break;
        }

        File MyFile = root.openNextFile ();

        while (MyFile)
        {
            if(MyFile.isDirectory())
            {
                // not a file we are looking for
                MyFile.close();
                MyFile = root.openNextFile();
                continue;
            }

            String EntryName = String (MyFile.name());
            EntryName = EntryName.substring ((('/' == EntryName[0]) ? 1 : 0));
            // DEBUG_V ("EntryName: '" + EntryName + "'");
            // DEBUG_V ("EntryName.length(): " + String(EntryName.length ()));

            if ((!EntryName.isEmpty ()) &&
                (0 != MyFile.size ())
               )
            {
                // DEBUG_V ("Adding File: '" + EntryName + "'");
                Response.push_back(EntryName);
                logcon ("'" + EntryName + "': \t'" + String (MyFile.size ()) + "'");
            }

            MyFile = root.openNextFile ();
        }
#else
        FsFile Entry;
        FsFile dir;
        ESP_SD.chdir(); // Set to sd root
        if(!dir.open ("/", O_READ))
        {
            logcon("ERROR:GetListOfSdFiles: Could not open root dir");
            break;
        }

        while (OpenNextSdEntry(Entry, dir))
        {
            if(!Entry.isFile() || Entry.isHidden())
            {
                // not a file we are looking for
                Entry.close();
                continue;
            }

            memset(entryName, 0x0, sizeof(entryName));
            Entry.getName(entryName, sizeof(entryName)-1);
            String EntryName = String (entryName);
            EntryName = EntryName.substring ((('/' == EntryName[0]) ? 1 : 0));
            // DEBUG_V ("EntryName: '" + EntryName + "'");
            // DEBUG_V ("EntryName.length(): " + String(EntryName.length ()));

            if ((!EntryName.isEmpty ()) &&
                (!EntryName.equals(String (F ("System Volume Information")))) &&
                (0 != Entry.size ())
               )
            {
                // DEBUG_V ("Adding File: '" + EntryName + "'");
                Response.push_back(EntryName);
            }

            Entry.close ();
        }

        dir.close();
#endif // ndef SUPPORT_SD
    } while (false);
    UnLockSd();

    // DEBUG_END;
} // GetListOfSdFiles

//-----------------------------------------------------------------------------
void c_FileMgr::printDirectory (FsFile & dir, int numTabs)
{
    // DEBUG_START;

    char entryName[256];
    FsFile Entry;
    String tabs = emptyString;
    tabs.reserve(2 * (numTabs + 1));
    for (uint8_t i = 0; i < numTabs; i++)
    {
        tabs = tabs + "  ";
    }

    while (OpenNextSdEntry(Entry, dir))
    {
        FeedWDT();

        memset(entryName, 0x0, sizeof(entryName));
        Entry.getName(entryName, sizeof(entryName)-1);
        // DEBUG_V(String("entryName: '") + String(entryName) + "'");

        if (Entry.isDirectory ())
        {
            logcon (tabs + F ("> ") + entryName);
            printDirectory (dir, numTabs + 1);
            Entry.close();
            continue;
        }

        if(!Entry.isHidden())
        {
            // files have sizes, directories do not
            logcon (tabs + entryName + F (" - ") + int64String(Entry.size ()));
        }
        Entry.close();
    }

    // DEBUG_END;
} // printDirectory

//-----------------------------------------------------------------------------
void c_FileMgr::SaveSdFile (const String & FileName, String & FileData)
{
    // DEBUG_START;

    do // once
    {
        FileId FileHandle = INVALID_FILE_HANDLE;
        if (false == OpenSdFile (FileName, FileMode::FileWrite, FileHandle, -1))
        {
            logcon (String (F ("Could not open '")) + FileName + F ("' for writting."));
            break;
        }

        int WriteCount = WriteSdFile (FileHandle, (byte*)FileData.c_str (), (uint64_t)FileData.length ());
        logcon (String (F ("Wrote '")) + FileName + F ("' ") + String(WriteCount));

         // DEBUG_FILE_HANDLE (FileHandle);
        CloseSdFile (FileHandle);

    } while (false);

    // DEBUG_END;

} // SaveSdFile

//-----------------------------------------------------------------------------
void c_FileMgr::SaveSdFile (const String & FileName, JsonVariant & FileData)
{
    // DEBUG_START;
    String Temp;
    serializeJson (FileData, Temp);
    SaveSdFile (FileName, Temp);
    // DEBUG_END;
} // SaveSdFile

//-----------------------------------------------------------------------------
bool c_FileMgr::OpenSdFile (const String & _FileName, FileMode Mode, FileId & FileHandle, int FileListIndex)
{
    // DEBUG_START;
    // DEBUG_V(String("Mode: ") + String(Mode));

    bool FileIsOpen = false;
    #ifdef SIMULATE_SD
    String FileName = String(CN_slashsdslash) + _FileName;
    #else
    String FileName = _FileName;
    #endif
    do // once
    {
        if (!SdCardInstalled)
        {
            // no SD card is installed
            FileHandle = INVALID_FILE_HANDLE;
            break;
        }

        // DEBUG_V ();

        // DEBUG_V (String("FileName: '") + FileName + "'");

        if (FileMode::FileRead == Mode)
        {
            // DEBUG_V (String("Read mode"));
            if (false == ESP_SD.exists (FileName))
            {
                logcon (String (F ("ERROR: Cannot find '")) + FileName + F ("' for reading. File does not exist."));
                break;
            }
        }
        // DEBUG_V ("File Exists");

        // do we have a pre defined index?
        if(-1 == FileListIndex)
        {
            // DEBUG_V("No predefined File Handle");
            FileHandle = CreateSdFileHandle ();
            // DEBUG_V (String("FileHandle: ") + String(FileHandle));

            FileListIndex = FileListFindSdFileHandle (FileHandle);
            // DEBUG_V(String("Using lookup File Index: ") + String(FileListIndex));
        }
        else
        {
            // DEBUG_V(String("Using predefined File Index: ") + String(FileListIndex));
            FileHandle = FileList[FileListIndex].handle;
        }

        // DEBUG_V("did we get an index");
        if (-1 != FileListIndex)
        {
            // DEBUG_V(String("Valid FileListIndex: ") + String(FileListIndex));
            FileList[FileListIndex].Filename = FileName;
            // DEBUG_V(String("Got file handle: ") + String(FileHandle));
            LockSd();
            #ifdef SIMULATE_SD
            FileList[FileListIndex].fsFile = ESP_SDFS.open (FileName, &XlateFileMode[Mode]);
            FileList[FileListIndex].IsOpen = true;
            #else
            FileList[FileListIndex].IsOpen = FileList[FileListIndex].fsFile.open(FileList[FileListIndex].Filename.c_str(), XlateFileMode[Mode]);
            #endif // def SIMULATE_SD
            UnLockSd();

            // DEBUG_V(String("ReadWrite: ") + String(XlateFileMode[Mode]));
            // xDEBUG_V(String("File Size: ") + String64(FileList[FileListIndex].fsFile.size()));
            FileList[FileListIndex].mode = Mode;

            // DEBUG_V("Open return");
            if (!FileList[FileListIndex].IsOpen)
            {
                logcon(String(F("ERROR: Could not open '")) + FileName + F("'."));
                // release the file list entry
                 // DEBUG_FILE_HANDLE (FileHandle);
                FileList[FileListIndex].IsOpen = false;
                CloseSdFile(FileHandle);
                break;
            }
            // DEBUG_V("");

            LockSd();
            FileList[FileListIndex].size = FileList[FileListIndex].fsFile.size();
            UnLockSd();
            // DEBUG_V(String(FileList[FileListIndex].Filename) + " - " + String(FileList[FileListIndex].size));

            if (FileMode::FileWrite == Mode)
            {
                // DEBUG_V ("Write Mode");
                LockSd();
                FileList[FileListIndex].fsFile.seek (0);
                UnLockSd();
            }
            // DEBUG_V ();

            FileIsOpen = true;
            // DEBUG_V (String ("File.Handle: ") + String (FileList[FileListIndex].handle));
        }
        else
        {
            // DEBUG_V("Could not get a file list index");
        }

    } while (false);

    // DEBUG_END;
    return FileIsOpen;

} // OpenSdFile

//-----------------------------------------------------------------------------
bool c_FileMgr::ReadSdFile (const String & FileName, String & FileData)
{
    // DEBUG_START;

   bool GotFileData = false;
    FileId FileHandle = INVALID_FILE_HANDLE;

    // DEBUG_V (String("File '") + FileName + "' is being opened.");
    if (true == OpenSdFile (FileName, FileMode::FileRead, FileHandle, -1))
    {
        // DEBUG_V (String("File '") + FileName + "' is open.");
        int FileListIndex;
        if (-1 != (FileListIndex = FileListFindSdFileHandle (FileHandle)))
        {
            LockSd();
            FileList[FileListIndex].fsFile.seek (0);
            FileData = FileList[FileListIndex].fsFile.readString ();
            UnLockSd();
        }

         // DEBUG_FILE_HANDLE (FileHandle);
        CloseSdFile (FileHandle);
        GotFileData = (0 != FileData.length());

        // DEBUG_V (FileData);
    }
    else
    {
         // DEBUG_FILE_HANDLE (FileHandle);
        CloseSdFile (FileHandle);
        logcon (String (F ("SD file: '")) + FileName + String (F ("' not found.")));
    }

    // DEBUG_END;
    return GotFileData;

} // ReadSdFile

//-----------------------------------------------------------------------------
bool c_FileMgr::ReadSdFile (const String & FileName, JsonDocument & FileData)
{
    // DEBUG_START;

    bool GotFileData = false;
    FileId FileHandle = INVALID_FILE_HANDLE;

    // DEBUG_V (String("File '") + FileName + "' is being opened.");
    if (true == OpenSdFile (FileName, FileMode::FileRead, FileHandle, -1))
    {
        // DEBUG_V (String("File '") + FileName + "' is open.");
        int FileListIndex;
        if (-1 != (FileListIndex = FileListFindSdFileHandle (FileHandle)))
        {
            LockSd();
            FileList[FileListIndex].fsFile.seek (0);
            String RawFileData = FileList[FileListIndex].fsFile.readString ();
            UnLockSd();

            // DEBUG_V ("Convert File to JSON document");
            DeserializationError error = deserializeJson (FileData, RawFileData);

            // DEBUG_V ("Error Check");
            if (error)
            {
                String CfgFileMessagePrefix = String (CN_Configuration_File_colon) + "'" + FileName + "' ";
                logcon (CN_Heap_colon + String (ESP.getFreeHeap ()));
                logcon (CfgFileMessagePrefix + String (F ("Deserialzation Error. Error code = ")) + error.c_str ());
                logcon (CN_plussigns + RawFileData + CN_minussigns);
            }
            else
            {
                GotFileData = true;
            }
        }
         // DEBUG_FILE_HANDLE (FileHandle);
        CloseSdFile(FileHandle);
    }
    else
    {
        logcon (String (F ("SD file: '")) + FileName + String (F ("' not found.")));
    }

    // DEBUG_END;
    return GotFileData;

} // ReadSdFile

//-----------------------------------------------------------------------------
uint64_t c_FileMgr::ReadSdFile (const FileId& FileHandle, byte* FileData, uint64_t NumBytesToRead, uint64_t StartingPosition)
{
    // DEBUG_START;

    uint64_t response = 0;

    // DEBUG_V (String ("       FileHandle: ") + String (FileHandle));
    // DEBUG_V (String ("   NumBytesToRead: ") + String (NumBytesToRead));
    // DEBUG_V (String (" StartingPosition: ") + String (StartingPosition));

    int FileListIndex;
    if (-1 != (FileListIndex = FileListFindSdFileHandle (FileHandle)))
    {
        uint64_t BytesRemaining = uint64_t(FileList[FileListIndex].size - StartingPosition);
        uint64_t ActualBytesToRead = min(NumBytesToRead, BytesRemaining);
        // DEBUG_V(String("   BytesRemaining: ") + String(BytesRemaining));
        // DEBUG_V(String("ActualBytesToRead: ") + String(ActualBytesToRead));
        LockSd();
        FileList[FileListIndex].fsFile.seek(StartingPosition);
        #ifdef SIMULATE_SD
        response = FileList[FileListIndex].fsFile.readBytes((char*)FileData, ActualBytesToRead);
        #else
        response = FileList[FileListIndex].fsFile.readBytes(FileData, ActualBytesToRead);
        #endif // def SIMULATE_SD
        UnLockSd();
        // DEBUG_V(String("         response: ") + String64(response));
    }
    else
    {
        logcon (String (F ("ReadSdFile::ERROR::Invalid File Handle: ")) + String (FileHandle));
    }

    // DEBUG_END;
    return response;

} // ReadSdFile

//-----------------------------------------------------------------------------
void c_FileMgr::CloseSdFile (FileId& FileHandle)
{
    // DEBUG_START;
    // DEBUG_V(String("      FileHandle: ") + String(FileHandle));

    int FileListIndex;
    if (-1 != (FileListIndex = FileListFindSdFileHandle (FileHandle)))
    {
        // DEBUG_V("Close the file");
        LockSd();
        FileList[FileListIndex].fsFile.close ();
        UnLockSd();

        FileList[FileListIndex].IsOpen = false;
        FileList[FileListIndex].handle = INVALID_FILE_HANDLE;

        if (nullptr != FileList[FileListIndex].buffer.DataBuffer)
        {
            if(FileList[FileListIndex].buffer.DataBuffer != OutputMgr.GetBufferAddress())
            {
                // only free the buffer if it is not malloc'd
                free(FileList[FileListIndex].buffer.DataBuffer);
            }
        }
        FileList[FileListIndex].buffer.DataBuffer = nullptr;
        FileList[FileListIndex].buffer.size = 0;
        FileList[FileListIndex].buffer.offset = 0;
    }
    else
    {
        logcon (String (F ("CloseSdFile::ERROR::Invalid File Handle: ")) + String (FileHandle));
    }

    FileHandle = INVALID_FILE_HANDLE;

    // DEBUG_END;

} // CloseSdFile

//-----------------------------------------------------------------------------
uint64_t c_FileMgr::WriteSdFile (const FileId& FileHandle, byte* FileData, uint64_t NumBytesToWrite)
{
    // DEBUG_START;

    uint64_t NumBytesWritten = 0;
    do // once
    {
        int FileListIndex;
        // DEBUG_V (String("Bytes to write: ") + String(NumBytesToWrite));
        if (-1 == (FileListIndex = FileListFindSdFileHandle (FileHandle)))
        {
            // DEBUG_V (String("FileHandle: ") + String(FileHandle));
            logcon (String (F ("WriteSdFile::ERROR::Invalid File Handle: ")) + String (FileHandle));
            break;
        }

        // DEBUG_V (String("FileListIndex: ") + String(FileListIndex));
        delay(10);
        FeedWDT();
        LockSd();
        // DEBUG_V();
        NumBytesWritten = FileList[FileListIndex].fsFile.write((uint8_t*)FileData, NumBytesToWrite);
        // DEBUG_V();
#ifdef SIMULATE_SD
        FileList[FileListIndex].fsFile.flush();
        const bool syncOk = !FileList[FileListIndex].fsFile.getWriteError();
#else
        const bool syncOk = FileList[FileListIndex].fsFile.sync() && !FileList[FileListIndex].fsFile.getWriteError();
#endif
        // DEBUG_V();
        UnLockSd();
        FeedWDT();
        delay(10);
        // DEBUG_V (String (" FileHandle: ") + String (FileHandle));
        // DEBUG_V (String ("File.Handle: ") + String (FileList[FileListIndex].handle));

        if(NumBytesWritten != NumBytesToWrite || !syncOk)
        {
            logcon(String(F("ERROR: SD Write failed. Tried writting ")) + String(NumBytesToWrite) + F(" bytes. Actually wrote ") + String(NumBytesWritten) + F(" bytes."));
                    NumBytesWritten = 0;
                    break;
        }
    } while(false);

    // DEBUG_END;
    return NumBytesWritten;

} // WriteSdFile

//-----------------------------------------------------------------------------
/*
    Specialized function that buffers data until the buffer is
    full and then writes out the buffer as a single operation.
*/
uint64_t c_FileMgr::WriteSdFileBuf (const FileId& FileHandle, byte* FileData, uint64_t NumBytesInSourceBuffer)
{
    const int fileIndex = FileListFindSdFileHandle(FileHandle);
    if (fileIndex < 0 || (NumBytesInSourceBuffer && FileData == nullptr))
    {
        return NumBytesInSourceBuffer ? 0 : UINT64_MAX;
    }
    if (NumBytesInSourceBuffer == 0)
    {
        // A zero-length call explicitly flushes; distinguish errors from success.
        return FlushSdFileBuffer(FileHandle) ? 0 : UINT64_MAX;
    }
    auto & buffer = FileList[fileIndex].buffer;
    if (buffer.DataBuffer == nullptr || buffer.size == 0)
    {
        return WriteSdFile(FileHandle, FileData, NumBytesInSourceBuffer);
    }
    uint64_t copied = 0;
    while (copied < NumBytesInSourceBuffer)
    {
        if (buffer.offset >= buffer.size && !FlushSdFileBuffer(FileHandle))
        {
            return 0;
        }
        const size_t chunk = size_t(min(buffer.size - buffer.offset, NumBytesInSourceBuffer - copied));
        memcpy(buffer.DataBuffer + buffer.offset, FileData + copied, chunk);
        buffer.offset += chunk;
        copied += chunk;
        FeedWDT();
    }
    return copied;
} // WriteSdFileBuf

bool c_FileMgr::FlushSdFileBuffer(const FileId & FileHandle)
{
    const int fileIndex = FileListFindSdFileHandle(FileHandle);
    if (fileIndex < 0)
    {
        return false;
    }
    auto & entry = FileList[fileIndex];
    auto & buffer = entry.buffer;
    FeedWDT();
    LockSd();
    bool success = true;
    if (buffer.offset)
    {
        success = buffer.DataBuffer != nullptr && buffer.offset <= buffer.size;
        if (success)
        {
            success = entry.fsFile.write(buffer.DataBuffer, size_t(buffer.offset)) == buffer.offset;
        }
    }
#ifdef SIMULATE_SD
    entry.fsFile.flush();
    success = success && !entry.fsFile.getWriteError();
#else
    success = entry.fsFile.sync() && !entry.fsFile.getWriteError() && success;
#endif
    UnLockSd();
    if (success)
    {
        buffer.offset = 0;
    }
    else
    {
        logcon(F("SD upload buffer flush failed"));
    }
    FeedWDT();
    return success;
}

//-----------------------------------------------------------------------------
uint64_t c_FileMgr::WriteSdFile (const FileId& FileHandle, byte* FileData, uint64_t NumBytesToWrite, uint64_t StartingPosition)
{
    // DEBUG_START;
    uint64_t response = 0;
    int FileListIndex;
    if (-1 != (FileListIndex = FileListFindSdFileHandle (FileHandle)))
    {
        // DEBUG_V (String (" NumBytesToWrite: ") + String (NumBytesToWrite));
        // DEBUG_V (String ("StartingPosition: ") + String (StartingPosition));
        // DEBUG_V (String ("      FileHandle: ") + String (FileHandle));
        // DEBUG_V (String ("     File.Handle: ") + String (FileList[FileListIndex].handle));
        LockSd();
        FileList[FileListIndex].fsFile.seek (StartingPosition);
        UnLockSd();
        response = WriteSdFile (FileHandle, FileData, NumBytesToWrite, true);
    }
    else
    {
        logcon (String (F ("WriteSdFile::ERROR::Invalid File Handle: ")) + String (FileHandle));
    }

    // DEBUG_END;
    return response;

} // WriteSdFile

//-----------------------------------------------------------------------------
uint64_t c_FileMgr::GetSdFileSize (const String& FileName)
{
    // DEBUG_START;
    uint64_t response = 0;
    FileId Handle = INVALID_FILE_HANDLE;
    if(OpenSdFile (FileName, FileMode::FileRead, Handle, -1))
    {
        response = GetSdFileSize(Handle);
        // DEBUG_FILE_HANDLE (Handle);
        CloseSdFile(Handle);
    }
    else
    {
        logcon(String(F("Could not open '")) + FileName + F("' to check size."));
    }
    // DEBUG_END;
    return response;

} // GetSdFileSize

//-----------------------------------------------------------------------------
uint64_t c_FileMgr::GetSdFileSize (const FileId& FileHandle)
{
    // DEBUG_START;

    uint64_t response = 0;
    int FileListIndex;
    if (-1 != (FileListIndex = FileListFindSdFileHandle (FileHandle)))
    {
        LockSd();
        response = FileList[FileListIndex].fsFile.size ();
        UnLockSd();
    }
    else
    {
        logcon (String (F ("GetSdFileSize::ERROR::Invalid File Handle: ")) + String (FileHandle));
    }
    // DEBUG_V(String("response: ") + int64String(response));

    // DEBUG_END;
    return response;

} // GetSdFileSize

//-----------------------------------------------------------------------------
void c_FileMgr::RenameSdFile(const String & OldName, const String & NewName)
{
    if (OldName.equalsIgnoreCase(NewName))
    {
        return;
    }
    // DEBUG_START;
    // DEBUG_V(String("OldName: '") + OldName + "'");
    // DEBUG_V(String("NewName: '") + NewName + "'");

#ifdef SIMULATE_SD
    RenameFlashFile(String(CN_slashsdslash) + OldName, String(CN_slashsdslash) + NewName);
#else
    LockSd();
    // only do this in the root dir
    ESP_SD.chdir();
    // rename does not work if the target already exists
    ESP_SD.remove(NewName);
    if(!ESP_SD.rename(OldName, NewName))
    {
        logcon(String(CN_stars) + F("Could not rename '") + OldName + F("' to '") + NewName + F("'") + CN_stars);
    }
    UnLockSd();
#endif // ndef SIMULATE_SD
    // DEBUG_END;
} // RenameSdFile

// Replace a completed staged file without discarding the previous good copy.
// If power or a rename fails, the previous file remains at TargetName or .unzip.bak.
bool c_FileMgr::ReplaceSdFile(const String & TempName, const String & TargetName)
{
    if (!SdCardInstalled || TempName.isEmpty() || TargetName.isEmpty())
    {
        return false;
    }
#ifdef SIMULATE_SD
    const String source = String(CN_slashsdslash) + TempName;
    const String target = String(CN_slashsdslash) + TargetName;
#else
    const String source = TempName;
    const String target = TargetName;
#endif
    const String backup = target + F(".unzip.bak");
    auto normalizedPath = [](String path)
    {
        path.replace("\\", "/");
        path.toLowerCase();
        String normalized;
        unsigned position = 0;
        while (position < path.length())
        {
            const int slash = path.indexOf('/', position);
            const unsigned end = slash < 0 ? unsigned(path.length()) : unsigned(slash);
            String component = path.substring(position, end);
            while (component.startsWith(" ")) component.remove(0, 1);
            while (component.endsWith(" ")) component.remove(unsigned(component.length() - 1));
            if (component == "..")
            {
                const int parent = normalized.lastIndexOf('/');
                normalized.remove(parent < 0 ? 0 : unsigned(parent));
            }
            else if (component != ".")
            {
                while (component.endsWith(".") || component.endsWith(" "))
                {
                    component.remove(unsigned(component.length() - 1));
                }
                if (!component.isEmpty())
                {
                    if (!normalized.isEmpty()) normalized += '/';
                    normalized += component;
                }
            }
            position = end + 1;
        }
        return normalized;
    };
    const String normalizedTarget = normalizedPath(target);
    const String normalizedBackup = normalizedPath(backup);
    LockSd();
#if !defined(SIMULATE_SD) && !defined(SUPPORT_SD_MMC)
    ESP_SD.chdir();
#endif
    bool success = false;
    do
    {
        bool fileInUse = false;
        for (const auto & entry : FileList)
        {
            if (!entry.IsOpen) continue;
            const String openedName = normalizedPath(entry.Filename);
            if (openedName == normalizedTarget || openedName == normalizedBackup)
            {
                logcon(String(F("Cannot replace open SD file: ")) + entry.Filename);
                fileInUse = true;
                break;
            }
        }
        if (fileInUse) break;
        if (source.equalsIgnoreCase(target))
        {
            success = ESP_SD.exists(source.c_str());
            break;
        }
        if (!ESP_SD.exists(source.c_str()))
        {
            break;
        }
        // Recover a previous interrupted replacement before starting another.
        if (ESP_SD.exists(backup.c_str()))
        {
            if (ESP_SD.exists(target.c_str()) || !ESP_SD.rename(backup.c_str(), target.c_str()))
            {
                logcon(String(F("Preserving recovery file: ")) + backup);
                break;
            }
        }
        const bool hadTarget = ESP_SD.exists(target.c_str());
        if (hadTarget && !ESP_SD.rename(target.c_str(), backup.c_str()))
        {
            break;
        }
        if (!ESP_SD.rename(source.c_str(), target.c_str()))
        {
            if (hadTarget && !ESP_SD.rename(backup.c_str(), target.c_str()))
            {
                logcon(String(F("Previous sequence preserved in: ")) + backup);
            }
            break;
        }
        success = true;
        if (hadTarget && !ESP_SD.remove(backup.c_str()))
        {
            logcon(String(F("Could not remove recovery file: ")) + backup);
        }
    } while (false);
    UnLockSd();
    return success;
}

//-----------------------------------------------------------------------------
void c_FileMgr::BuildFseqList(bool DisplayFileNames)
{
    // DEBUG_START;

    char entryName [256];
    do // once
    {
        if(!SdCardIsInstalled())
        {
            // DEBUG_V("No SD card installed.");
            BuildDefaultFseqList();
            break;
        }

        FeedWDT();

        JsonDocument jsonDoc;
        jsonDoc.to<JsonObject>();

        JsonWrite(jsonDoc, "totalBytes", SdCardSize);
        JsonArray jsonDocFileList = jsonDoc["files"].to<JsonArray> ();

        uint32_t FileIndex = 0;
        uint64_t usedBytes = 0;
        uint32_t numFiles = 0;

        LockSd();
#ifdef SIMULATE_SD
        JsonWrite(jsonDoc, "usedBytes", ESP_SD.usedBytes());
        File InputDir = ESP_SD.open (CN_slashsd, CN_r);
#else
        ESP_SD.chdir();
        FsFile InputDir = ESP_SD.open ("/", O_READ);
        ESP_SD.chdir();
#endif // ndef SIMULATE_SD
        if(!InputDir)
        {
            UnLockSd();
            logcon(F("ERROR: Could not open SD card for Reading FSEQ List."));
            break;
        }

        // DEBUG_V(String("InputDir Name: ") + String(InputDir.name()));

#ifdef SIMULATE_SD
        String CurrentEntryName = InputDir.getNextFileName();
        while (!CurrentEntryName.isEmpty())
        {
            // DEBUG_V("Process a file entry");
            // DEBUG_V(String("CurrentEntryName: ") + CurrentEntryName);

            FeedWDT();

            File CurrentEntry = ESP_SD.open(CurrentEntryName, CN_r);
            if(CurrentEntry.isDirectory())
            {
                // DEBUG_V("Skip embedded directory and hidden files");
                CurrentEntry.close();
                CurrentEntry = InputFile.openNextFile();
                continue;
            }

            String EntryName = String (CurrentEntry.name());
            // DEBUG_V (          "EntryName: " + EntryName);
            // DEBUG_V (" EntryName.length(): " + String(EntryName.length ()));
            // DEBUG_V ("CurrentEntry.size(): " + String(CurrentEntry.size ()));

            if ((!EntryName.isEmpty ()) && (0 != CurrentEntry.size ()))
            {
                FoundZipFile |= IsCompressed(EntryName);

                usedBytes += CurrentEntry.size ();
                ++numFiles;

                if(DisplayFileNames)
                {
                    logcon (String(F("SD File: '")) + EntryName + "'   " + String(CurrentEntry.size ()));
                }
                uint16_t Date;
                uint16_t Time;
                // CurrentEntry.getCreateDateTime (&Date, &Time);
                // DEBUG_V(String("Date: ") + String(Date));
                // DEBUG_V(String("Year: ") + String(FS_YEAR(Date)));
                // DEBUG_V(String("Day: ") + String(FS_DAY(Date)));
                // DEBUG_V(String("Month: ") + String(FS_MONTH(Date)));

                // DEBUG_V(String("Time: ") + String(Time));
                // DEBUG_V(String("Hours: ") + String(FS_HOUR(Time)));
                // DEBUG_V(String("Minutes: ") + String(FS_MINUTE(Time)));
                // DEBUG_V(String("Seconds: ") + String(FS_SECOND(Time)));

                tmElements_t tm;
                tm.Year = FS_YEAR(Date) - 1970;
                tm.Month = FS_MONTH(Date);
                tm.Day = FS_DAY(Date);
                tm.Hour = FS_HOUR(Time);
                tm.Minute = FS_MINUTE(Time);
                tm.Second = FS_SECOND(Time);

                // DEBUG_V(String("tm: ") + String(time_t(makeTime(tm))));
                jsonDocFileList[FileIndex]["name"] = EntryName;
                jsonDocFileList[FileIndex]["date"] = makeTime(tm);
                jsonDocFileList[FileIndex]["length"] = CurrentEntry.size ();
                ++FileIndex;
            }
            else
            {
                // DEBUG_V("Skipping File");
            }
            CurrentEntry.close();
            CurrentEntryName = InputDir.getNextFileName ();

        } // end while true
    #else
        FsFile CurrentEntry;
        while (OpenNextSdEntry(CurrentEntry, InputDir))
                {
            // DEBUG_V("Process a file entry");
            FeedWDT();

            if(CurrentEntry.isDirectory() || CurrentEntry.isHidden())
            {
                // DEBUG_V("Skip embedded directory and hidden files");
                CurrentEntry.close();
                continue;
            }

            memset(entryName, 0x0, sizeof(entryName));
            CurrentEntry.getName (entryName, sizeof(entryName)-1);
            String EntryName = String (entryName);
            // DEBUG_V (         "EntryName: " + EntryName);
            // DEBUG_V ("EntryName.length(): " + String(EntryName.length ()));
            // DEBUG_V ("      entry.size(): " + int64String(CurrentEntry.size ()));

            if ((!EntryName.isEmpty ()) &&
                (0 != CurrentEntry.size ())
               )
            {
                FoundZipFile |= IsCompressed(EntryName);

                usedBytes += CurrentEntry.size ();
                ++numFiles;

                if(DisplayFileNames)
                {
                    logcon (String(F("SD File: '")) + EntryName + "'   " + String(CurrentEntry.size ()));
                }
                uint16_t Date;
                uint16_t Time;
                CurrentEntry.getCreateDateTime (&Date, &Time);
                // DEBUG_V(String("Date: ") + String(Date));
                // DEBUG_V(String("Year: ") + String(FS_YEAR(Date)));
                // DEBUG_V(String("Day: ") + String(FS_DAY(Date)));
                // DEBUG_V(String("Month: ") + String(FS_MONTH(Date)));

                // DEBUG_V(String("Time: ") + String(Time));
                // DEBUG_V(String("Hours: ") + String(FS_HOUR(Time)));
                // DEBUG_V(String("Minutes: ") + String(FS_MINUTE(Time)));
                // DEBUG_V(String("Seconds: ") + String(FS_SECOND(Time)));

                tmElements_t tm;
                tm.Year = FS_YEAR(Date) - 1970;
                tm.Month = FS_MONTH(Date);
                tm.Day = FS_DAY(Date);
                tm.Hour = FS_HOUR(Time);
                tm.Minute = FS_MINUTE(Time);
                tm.Second = FS_SECOND(Time);

                // DEBUG_V(String("tm: ") + String(time_t(makeTime(tm))));
                jsonDocFileList[FileIndex]["name"] = EntryName;
                jsonDocFileList[FileIndex]["date"] = makeTime(tm);
                jsonDocFileList[FileIndex]["length"] = CurrentEntry.size ();
                ++FileIndex;
            }
            else
            {
                // DEBUG_V("Skipping File");
            }
            CurrentEntry.close();
        } // end while true

        JsonWrite(jsonDoc, "usedBytes", usedBytes);

    #endif // ndef SIMULATE_SD

        InputDir.close();
        UnLockSd();

        // close the array and add the descriptive data
        JsonWrite(jsonDoc, "numFiles", numFiles);
        JsonWrite(jsonDoc, "SdCardPresent", true);
        PrettyPrint (jsonDoc, String ("FSEQ File List"));
        SaveFlashFile(String(CN_fseqfilelist) + F(".json"), jsonDoc);
    } while(false);

    // String Temp;
    // ReadFlashFile(FSEQFILELIST, Temp);
    // xDEBUG_V(Temp);

    // DEBUG_END;

} // BuildFseqList

//-----------------------------------------------------------------------------
void c_FileMgr::FindFirstZipFile(String &FileName)
{
    // DEBUG_START;
    char entryName [256];

    do // once
    {
        if(!SdCardIsInstalled())
        {
            // DEBUG_V("No SD card installed.");
            break;
        }

        FeedWDT();
        LockSd();
#ifdef SIMULATE_SD
        File InputFile;
        InputFile = ESP_SD.open(CN_slashsd, CN_r);
        if(!InputFile)
#else
        FsFile InputFile;
        ESP_SD.chdir();
        if(!InputFile.open ("/", O_READ))
#endif // ndef SIMULATE_SD
        {
            UnLockSd();
            logcon(F("ERROR: Could not open SD card for Reading FSEQ List."));
            break;
        }

        // open output file, erase old data
        #ifdef SIMULATE_SD

        File CurrentEntry = InputFile.openNextFile();
        while (CurrentEntry)
        {
            // DEBUG_V("Process a file entry");
            FeedWDT();

            if(CurrentEntry.isDirectory())
            {
                // DEBUG_V("Skip embedded directory and hidden files");
                CurrentEntry.close();
                continue;
            }

            String EntryName = String (CurrentEntry.name());
            // DEBUG_V (         "EntryName: " + EntryName);
            // DEBUG_V ("EntryName.length(): " + String(EntryName.length ()));
            // DEBUG_V (String("      entry.size(): ") + int64String(CurrentEntry.size ()));

            // is this a zipped file?
            if(IsCompressed(EntryName))
            {
                FileName = EntryName;
                CurrentEntry.close();
                // InputFile.close();
                break;
            }
            else
            {
                // DEBUG_V("Skipping File");
            }
            CurrentEntry.close();
            CurrentEntry = InputFile.openNextFile();
        } // end while true
        #else
        ESP_SD.chdir();

        FsFile CurrentEntry;
        while (OpenNextSdEntry(CurrentEntry, InputFile))
        {
            // DEBUG_V("Process a file entry");
            FeedWDT();

            if(CurrentEntry.isDirectory() || CurrentEntry.isHidden())
            {
                // DEBUG_V("Skip embedded directory and hidden files");
                CurrentEntry.close();
                continue;
            }

            memset(entryName, 0x0, sizeof(entryName));
            CurrentEntry.getName (entryName, sizeof(entryName)-1);
            String EntryName = String (entryName);
            // DEBUG_V (         "EntryName: " + EntryName);
            // DEBUG_V ("EntryName.length(): " + String(EntryName.length ()));
            // DEBUG_V ("      entry.size(): " + int64String(CurrentEntry.size ()));

            // is this a zipped file?
            if(IsCompressed(EntryName))
            {
                FileName = EntryName;
                CurrentEntry.close();
                InputFile.close();
                break;
            }
            else
            {
                // DEBUG_V("Skipping File");
            }
            CurrentEntry.close();
        } // end while true

        #endif // ndef SIMULATE_SD

        InputFile.close();
        UnLockSd();
    } while(false);

    // DEBUG_END;

} // FindFirstZipFile

//-----------------------------------------------------------------------------
bool c_FileMgr::handleFileUpload (
    const String & filename,
    size_t index,
    uint8_t* data,
    size_t len,
    bool final,
    uint32_t totalLen)
{
    FeedWDT();
    if (index == 0)
    {
#ifdef SUPPORT_UNZIP
        if (gUnzipFiles.IsFileInUse(filename))
        {
            return false;
        }
#endif
        handleFileUploadNewFile(filename);
        expectedIndex = 0;
    }
    if (fsUploadFileHandle == INVALID_FILE_HANDLE || !fsUploadFileName.equals(filename) ||
        index != expectedIndex || len > UINT32_MAX - expectedIndex ||
        (len && WriteSdFileBuf(fsUploadFileHandle, data, len) != len))
    {
        logcon(String(F("Upload failed for: ")) + filename);
        AbortSdFileUpload();
        BuildFseqList(false);
        return false;
    }
    expectedIndex += uint32_t(len);
    if (!final)
    {
        return true;
    }

    // HTTP Content-Length includes multipart boundaries, so validate the SD
    // file against accepted payload bytes rather than the request body length.
    const bool flushed = FlushSdFileBuffer(fsUploadFileHandle);
    const uint32_t payloadSize = expectedIndex;
    CloseSdFile(fsUploadFileHandle);
    const uint64_t storedSize = GetSdFileSize(fsUploadFileName);
    const bool success = flushed && storedSize == payloadSize;
    if (success)
    {
        logcon(String(F("Upload File: '")) + fsUploadFileName + F("' Done (") +
               String((millis() - fsUploadStartTime) / 1000) + F("s). Received: ") +
               String(payloadSize) + F(" bytes. FileLen: ") + int64String(storedSize));
#ifdef SUPPORT_UNZIP
        if (IsCompressed(fsUploadFileName))
        {
            gUnzipFiles.Run();
        }
#endif
    }
    else
    {
        logcon(String(F("Upload failed size or flush validation: ")) + fsUploadFileName);
        DeleteSdFile(fsUploadFileName);
    }
    BuildFseqList(false);
    fsUploadFileName.clear();
    expectedIndex = 0;
    fsUploadInProgress = false;
    (void)totalLen;
    return success;
} // handleFileUpload

//-----------------------------------------------------------------------------
void c_FileMgr::handleFileUploadNewFile (const String & filename)
{
    // DEBUG_START;
    // DEBUG_V ("UploadStart: " + filename);

    fsUploadStartTime = millis();
    fsUploadInProgress = true;
    // DEBUG_V(String("filename: ") + String(filename));

    // are we terminating the previous download?
    if (!fsUploadFileName.isEmpty())
    {
        logcon (String (F ("Aborting Previous File Upload For: '")) + fsUploadFileName + String (F ("'")));
        // DEBUG_FILE_HANDLE (fsUploadFileHandle);
        if (fsUploadFileHandle != INVALID_FILE_HANDLE)
        {
            CloseSdFile(fsUploadFileHandle);
        }
        DeleteSdFile(fsUploadFileName);
    }

    // Set up to receive a file
    fsUploadFileName.clear();
    fsUploadFileName = filename;

    logcon (String (F ("Upload File: '")) + fsUploadFileName + String (F ("' Started")));

    DeleteSdFile (fsUploadFileName);

    // Open the file for writing
    // DEBUG_V(String("fsUploadFileName: ") + String(fsUploadFileName));
    OpenSdFile (fsUploadFileName, FileMode::FileWrite, fsUploadFileHandle, -1 /*first access*/);
    int FileListIndex;
    if (-1 == (FileListIndex = FileListFindSdFileHandle (fsUploadFileHandle)))
    {
        logcon (String (F ("WriteSdFileBuf::ERROR::Invalid File Handle: ")) + String (fsUploadFileHandle));
    }
    else
    {
        // DEBUG_V("Use the output buffer as a data buffer");
        FileList[FileListIndex].buffer.offset = 0;
        FileList[FileListIndex].buffer.size = min(uint32_t(OutputMgr.GetBufferSize() & ~(SD_BLOCK_SIZE - 1)), uint32_t(MAX_SD_BUFFER_SIZE));
        FileList[FileListIndex].buffer.DataBuffer = FileList[FileListIndex].buffer.size
            ? OutputMgr.GetBufferAddress() : nullptr;
        // DEBUG_V(String("Buffer Size: ") + String(FileList[FileListIndex].buffer.size));
    }

    // DEBUG_END;

} // handleFileUploadNewFile

//-----------------------------------------------------------------------------
void c_FileMgr::BuildDefaultFseqList ()
{
    // DEBUG_START;

    JsonDocument jsonDoc;
    jsonDoc.to<JsonObject>();
    JsonWrite(jsonDoc, "SdCardPresent", false);
    JsonWrite(jsonDoc, "totalBytes", 0);
    JsonWrite(jsonDoc, "usedBytes", 0);
    JsonWrite(jsonDoc, "numFiles", 0);
    jsonDoc["files"].to<JsonArray> ();
    SaveFlashFile(String(CN_fseqfilelist) + F(".json"), jsonDoc);

    // DEBUG_END;

    return;
} // BuildDefaultFseqList

//-----------------------------------------------------------------------------
bool c_FileMgr::SeekSdFile(const FileId & FileHandle, uint64_t position, SeekMode Mode)
{
    // DEBUG_START;

    // DEBUG_V(String("FileHandle: ") + String(FileHandle));
    // DEBUG_V(String("  position: ") + String(position));
    // DEBUG_V(String("      Mode: ") + String(uint32_t(Mode)));

    bool response = false;
    int FileListIndex;
    do // once
    {
        if (-1 == (FileListIndex = FileListFindSdFileHandle (FileHandle)))
        {
            logcon (String (F ("SeekSdFile::ERROR::Invalid File Handle: ")) + String (FileHandle));
            break;
        }

        LockSd();
        switch(Mode)
        {
            case SeekMode::SeekSet:
            {
                response = FileList[FileListIndex].fsFile.seek (position);
                break;
            }
            case SeekMode::SeekEnd:
            {
                uint64_t EndPosition = FileList[FileListIndex].fsFile.size();
                response = FileList[FileListIndex].fsFile.seek (EndPosition - position);
                break;
            }
            case SeekMode::SeekCur:
            {
                uint64_t CurrentPosition = FileList[FileListIndex].fsFile.position();
                response = FileList[FileListIndex].fsFile.seek (CurrentPosition + position);
                break;
            }
            default:
            {
                logcon("Procedural error. Cannot set seek value");
                break;
            }
        } // end switch mode
        UnLockSd();
    } while(false);

    // DEBUG_END;
    return response;

} // SeekSdFile

//-----------------------------------------------------------------------------
void c_FileMgr::LockSd()
{
    // DEBUG_START;

#ifdef ARDUINO_ARCH_ESP32
    xSemaphoreTake( SdAccessSemaphore, TickType_t(-1) );
#endif // def ARDUINO_ARCH_ESP32

    // DEBUG_END;
} // LockSd

//-----------------------------------------------------------------------------
void c_FileMgr::UnLockSd()
{
    // DEBUG_START;
#ifdef ARDUINO_ARCH_ESP32
    xSemaphoreGive( SdAccessSemaphore );
#endif // def ARDUINO_ARCH_ESP32

    // DEBUG_END;
} // UnLockSd

//-----------------------------------------------------------------------------
void c_FileMgr::AbortSdFileUpload()
{
    if (fsUploadFileHandle != INVALID_FILE_HANDLE)
    {
        CloseSdFile(fsUploadFileHandle);
    }
    if (!fsUploadFileName.isEmpty())
    {
        DeleteSdFile(fsUploadFileName);
    }
    fsUploadFileName.clear();
    expectedIndex = 0;
    fsUploadInProgress = false;
}

bool c_FileMgr::IsUploadInProgress() const
{
    return fsUploadInProgress || FPPDiscovery.IsUploadInProgress()
#ifdef SUPPORT_FTP
        || FtpUploadInProgress
#endif
        ;
}

//-----------------------------------------------------------------------------
bool c_FileMgr::IsCompressed(String FileName)
{
    // DEBUG_START;
    bool Response = false;

    // DEBUG_V(String("FileName: ") + FileName);
    FileName.toLowerCase();
    Response = FileName.endsWith(".zip") || FileName.endsWith(".xlz");
    // DEBUG_V(String("Response: ") + String(Response));

    // DEBUG_END;
    return Response;
} // IsCompressed

//-----------------------------------------------------------------------------
void c_FileMgr::GetSdInfo(SdInfo & Response)
{
    Response = SdInfo();
    if (!SdCardInstalled)
    {
        return;
    }
    FeedWDT();
    LockSd();
#ifdef SIMULATE_SD
#ifdef ARDUINO_ARCH_ESP32
    Response.MaxSize = LittleFS.totalBytes();
    Response.Used = LittleFS.usedBytes();
#else
    FSInfo info;
    if (LittleFS.info(info))
    {
        Response.MaxSize = info.totalBytes;
        Response.Used = info.usedBytes;
    }
#endif
    Response.Available = Response.MaxSize > Response.Used ? Response.MaxSize - Response.Used : 0;
#elif defined(SUPPORT_SD_MMC)
    Response.MaxSize = ESP_SD.totalBytes();
    Response.Used = ESP_SD.usedBytes();
    Response.Available = Response.MaxSize > Response.Used ? Response.MaxSize - Response.Used : 0;
#else
    const uint64_t bytesPerCluster = sd.bytesPerCluster();
    const uint32_t clusters = sd.clusterCount();
    const uint32_t freeClusters = sd.freeClusterCount();
    Response.MaxSize = uint64_t(clusters) * bytesPerCluster;
    // An invalid free-cluster result is deliberately treated as no room.
    Response.Available = freeClusters <= clusters ? uint64_t(freeClusters) * bytesPerCluster : 0;
    Response.Used = Response.MaxSize - Response.Available;
#endif
    UnLockSd();
    FeedWDT();
} // GetSdInfo

// create a global instance of the File Manager
c_FileMgr FileMgr;
