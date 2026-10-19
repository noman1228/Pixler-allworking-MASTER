/*
 * DisplayOLED.h
 *
 * Project: ESPixelStick - An ESP8266 / ESP32 and E1.31 based pixel driver
 * Copyright (c) 2018, 2025 Shelby Merrick
 * http://www.forkineye.com
 */

#ifndef DISPLAY_OLED_H
#define DISPLAY_OLED_H

#include "ESPixelStick.h"
#include "ConstNames.hpp"
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include "network/NetworkMgr.hpp"
#include "FPPDiscovery.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Constants for timing and display
#define OLED_TASK_DELAY_MS         250
#define TOAST_DURATION_MS          5000
#define NETWORK_UPDATE_INTERVAL_MS 10000
#define BUTTON_DEBOUNCE_DELAY_MS   1000

// Enum for OLED display pages
enum class DisplayPage
{
    NETWORK_INFO,
    RUNNING_STATUS,
    UPLOAD_STATUS
};

// NEW: which network icon was shown last when both Wi-Fi and Ethernet are up
enum class NetShown : uint8_t
{
    WIFI,
    ETH
};

class c_OLED
{
public:
    c_OLED();
    void Begin();
    void InitNow();
    void End();

    void Update(bool forceUpdate = false);
    void UpdateNetworkInfo(bool forceUpdate = false);
    void UpdateUploadStatus(const String &filename, int progress);
    void UpdateRunningStatus();

    void Flip();
    void Poll();
    void ShowToast(const String &message, const String &title = "");
    void ShowRebootScreen();

    // persisted/user-visible state
    bool   flipState = false;
    bool   isUploading = false;
    String uploadFilename;
    int    uploadProgress = 0;
    unsigned long lastUploadUpdate = 0;

    void GetDriverName(String &name) const { name = "OLED"; }

    // Button interrupt handler
    static void IRAM_ATTR buttonISR();

private:
    // toast state
    bool           isToastActive = false;
    unsigned long  toastStartTime = 0;
    String         toastMessage;

    // network view state
    NetShown lastNetShown = NetShown::WIFI;   // alternate when both links up
    String   dispNetMode;                     // "WIFI" | "ETH" | "NONE"
    String   dispIP;
    String   dispHostName;
    int      dispRSSI = 0;

    int getSignalStrength(int rssi);

    // page/refresh state
    DisplayPage   currentPage = DisplayPage::NETWORK_INFO;
    unsigned long lastPageSwitchTime = 0;
    unsigned long lastNetworkUpdate  = 0;
    const unsigned long pageSwitchInterval = 7500;

    Preferences preferences;
};

extern c_OLED OLED;

#endif // DISPLAY_OLED_H
