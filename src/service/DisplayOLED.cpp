#ifdef SUPPORT_OLED
#include "service/DisplayOLED.h"
#include <Preferences.h>
#include <vector>  // for std::vector used in DrawCenteredWrappedText

#if defined(ARDUINO_ARCH_ESP32)
  #include <ETH.h>     // for ETH.linkUp(), ETH.localIP()
#endif
#include <WiFi.h>       // for WiFi.isConnected(), WiFi.RSSI(), WiFi.localIP()

#define TOAST_DURATION_MS          5000
#define NETWORK_UPDATE_INTERVAL_MS 10000
#define BUTTON_DEBOUNCE_DELAY_MS   1000
#define VERSION_STRING             "4.x-dev"

static bool isRebooting = false;
static SemaphoreHandle_t displayMutex = nullptr;
static TaskHandle_t oledTaskHandle = nullptr;

// --- NEW: simple enum to remember which net we showed last when both are up

#ifndef ESP_SCL_PIN
#define ESP_SCL_PIN SCL
#endif
#ifndef ESP_SDA_PIN
#define ESP_SDA_PIN SDA
#endif

static U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, ESP_SCL_PIN, ESP_SDA_PIN);

// --- UPDATED: AP status page tuned for 128x32 and safe truncation ---
static void drawApStatus(U8G2 &u8g2)
{
    const String ssidIn = WiFi.softAPSSID();

    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_6x10_tr);

    // Line 1
    const char* line1 = "CONNECT TO AP:";
    int16_t x1 = (128 - u8g2.getStrWidth(line1)) / 2;
    u8g2.drawStr(x1, 12, line1);

    // Line 2 (SSID)
    int16_t x2 = (128 - u8g2.getStrWidth(ssidIn.c_str())) / 2;
    u8g2.drawStr(x2, 28, ssidIn.c_str());

    u8g2.sendBuffer();
}

static void OLEDTask(void *) {
    while (true) {
        if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            OLED.Poll();
            xSemaphoreGive(displayMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(OLED_TASK_DELAY_MS));
    }
}

c_OLED::c_OLED()
    : currentPage(DisplayPage::NETWORK_INFO), lastPageSwitchTime(0), lastNetworkUpdate(0),
      isUploading(false), uploadProgress(0), lastUploadUpdate(0), dispRSSI(0),
      isToastActive(false), toastStartTime(0), flipState(false), preferences()
    , lastNetShown(NetShown::WIFI) // NEW: start by showing Wi-Fi first
{}

void c_OLED::Begin() {
    displayMutex = xSemaphoreCreateMutex();
    if (!displayMutex) return;

    preferences.begin("oled", false);
    flipState = preferences.getBool("flipState", false);
    u8g2.setDisplayRotation(flipState ? U8G2_R2 : U8G2_R0);
    if (!u8g2.begin()) return;

    xTaskCreatePinnedToCore(OLEDTask, "OLEDTask", 2048, nullptr, 1, &oledTaskHandle, 0);

#ifdef BUTTON_GPIO1
    pinMode(BUTTON_GPIO1, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_GPIO1), buttonISR, FALLING);
#endif

    InitNow();
}

// ---------- helpers (NEW) ----------

static void drawWifiBars(U8G2 &g, int bars) {
    // Draw up to 4 bars at top-right
    bars = constrain(bars, 0, 4);
    for (int i = 0; i < bars; i++) {
        // x grows left->right, y grows down. Bar width=4, gap=2
        g.drawBox(105 + i * 6, 16 - (i + 1) * 4, 4, (i + 1) * 4);
    }
}
#ifndef WIFI_AP_NAME
#define WIFI_AP_NAME "ESPS-AP"
#endif


static void drawEthCord(U8G2 &g) {
    // A tiny RJ45 plug (right side) + a short “cord”
    // Plug body
    g.drawFrame(108, 2, 18, 12);         // outer body
    g.drawHLine(108, 6, 18);             // latch line
    // Gold pins
    for (int i = 0; i < 6; i++) {
        g.drawVLine(111 + i * 2, 8, 4);
    }
    // Cord (leftwards)
    g.drawHLine(96, 8, 12);
    g.drawHLine(96, 9, 12);
}

static bool getWiFiInfo(IPAddress &ip, int &rssi) {
    if (WiFi.isConnected()) {
        ip = WiFi.localIP();
        rssi = WiFi.RSSI();
        return ip != INADDR_NONE && ip.toString() != "0.0.0.0";
    }
    return false;
}

static bool getEthInfo(IPAddress &ip) {
#if defined(ARDUINO_ARCH_ESP32)
    if (ETH.linkUp()) {
        ip = ETH.localIP();
        return ip != INADDR_NONE && ip.toString() != "0.0.0.0";
    }
#endif
    return false;
}

static void DrawCenteredWrappedText(U8G2 &u8g2, const String &text, uint8_t maxWidth, uint8_t xOrigin, uint8_t yOrigin) {
    constexpr uint8_t DISPLAY_WIDTH  = 128;
    constexpr uint8_t DISPLAY_HEIGHT = 32;

    auto setFont = [&](bool large) {
        u8g2.setFont(large ? u8g2_font_10x20_tf : u8g2_font_6x10_tf);
    };

    std::vector<String> lines;
    uint8_t lineHeight = 0;
    bool fontFit = false;

    for (int attempt = 0; attempt < 2; attempt++) {
        setFont(attempt == 0);
        lines.clear();
        String currentLine, currentWord;

        for (uint16_t i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == ' ' || c == '\n') {
                if (u8g2.getStrWidth((currentLine + currentWord).c_str()) > maxWidth) {
                    lines.push_back(currentLine);
                    currentLine = currentWord + " ";
                } else {
                    currentLine += currentWord + " ";
                }
                currentWord = "";
                if (c == '\n') { lines.push_back(currentLine); currentLine = ""; }
            } else {
                currentWord += c;
            }
        }

        if (!currentWord.isEmpty()) {
            if (u8g2.getStrWidth((currentLine + currentWord).c_str()) > maxWidth) {
                lines.push_back(currentLine);
                currentLine = currentWord;
            } else {
                currentLine += currentWord;
            }
        }
        if (!currentLine.isEmpty()) lines.push_back(currentLine);

        lineHeight = u8g2.getAscent() - u8g2.getDescent() + 2;
        if (lineHeight * lines.size() <= DISPLAY_HEIGHT) {
            fontFit = true;
            break;
        }
    }

    if (!fontFit) {
        setFont(false);
        lineHeight = u8g2.getAscent() - u8g2.getDescent() + 2;
    }

    uint8_t totalHeight = lineHeight * lines.size();
    uint8_t startY = yOrigin + ((DISPLAY_HEIGHT - totalHeight) / 2);

    for (size_t i = 0; i < lines.size(); i++) {
        uint8_t strWidth = u8g2.getStrWidth(lines[i].c_str());
        uint8_t x = xOrigin + ((DISPLAY_WIDTH - strWidth) / 2);
        uint8_t y = startY + (i * lineHeight);
        u8g2.drawStr(x, y, lines[i].c_str());
    }
}

// ---------- unchanged init/draw splash ----------

void c_OLED::InitNow()
{
    if (!displayMutex || xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    u8g2.clearBuffer();
    u8g2.setFontMode(1);
    u8g2.setBitmapMode(1);
    u8g2.setFont(u8g2_font_t0_12b_tr);
    u8g2.drawStr(28, 28, "ESPixelStick");
    u8g2.setFont(u8g2_font_profont22_tr);
    u8g2.drawStr(10, 17, "INITIALZE");
    u8g2.setDrawColor(2);
    u8g2.drawFrame(0, 0, 128, 31);
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_4x6_tr);
    u8g2.drawStr(112, 27, "4.x");
    u8g2.drawStr(5, 27, "DEV");
    u8g2.sendBuffer();

    xSemaphoreGive(displayMutex);
}

void c_OLED::End()
{
    if (oledTaskHandle) { vTaskDelete(oledTaskHandle); oledTaskHandle = nullptr; }
    if (displayMutex) { vSemaphoreDelete(displayMutex); displayMutex = nullptr; }
    preferences.end();
}

// ---------- UPDATED: Network page logic ----------

void c_OLED::UpdateNetworkInfo(bool forceUpdate)
{
    if (!displayMutex || xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    // Gather current status
    IPAddress wifiIP, ethIP;
    int wifiRSSIraw = -127;
    const bool wifiUp = getWiFiInfo(wifiIP, wifiRSSIraw);
    const bool ethUp  = getEthInfo(ethIP);
    const bool apOn   = (WiFi.getMode() & WIFI_MODE_AP);

    // Hostname (unchanged)
    String currHost; NetworkMgr.GetHostname(currHost);

    // Decide what to show
    bool showWiFi = false;
    bool showEth  = false;
    bool showAp   = false;

    if (wifiUp && ethUp) {
        // alternate each refresh
        if (forceUpdate || (millis() - lastNetworkUpdate) >= NETWORK_UPDATE_INTERVAL_MS) {
            lastNetShown = (lastNetShown == NetShown::WIFI) ? NetShown::ETH : NetShown::WIFI;
        }
        showWiFi = (lastNetShown == NetShown::WIFI);
        showEth  = !showWiFi;
    } else if (wifiUp) {
        showWiFi = true;
        lastNetShown = NetShown::WIFI;
    } else if (ethUp) {
        showEth = true;
        lastNetShown = NetShown::ETH;
    } else if (apOn) {
        // Neither up, but AP is active -> show AP connection instructions
        showAp = true;
    }

    // Only redraw if something changed (IP/host/RSSI or toggled side) or forced
    String newDispIP;
    int newDispRSSI = dispRSSI;

    if (showWiFi) {
        newDispIP = wifiIP.toString();
        newDispRSSI = getSignalStrength(wifiRSSIraw);
    } else if (showEth) {
        newDispIP = ethIP.toString();
        newDispRSSI = 0; // not used for ETH
    } else if (showAp) {
        newDispIP = "0.0.0.0";
        newDispRSSI = 0;
    } else {
        newDispIP = "0.0.0.0";
        newDispRSSI = 0;
    }

    const bool toggledView =
        (showWiFi && dispNetMode != "WIFI") ||
        (showEth  && dispNetMode != "ETH")  ||
        (showAp   && dispNetMode != "AP")   ||
        (!showWiFi && !showEth && !showAp && dispNetMode != "NONE");

    if (forceUpdate || toggledView || newDispIP != dispIP || currHost != dispHostName || newDispRSSI != dispRSSI) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB08_tr);

        if (showWiFi) {
            // Icon + lines for Wi-Fi
            drawWifiBars(u8g2, newDispRSSI);
            String ipLine = "WIFI: " + newDispIP;
            u8g2.drawStr(0, 18, ipLine.c_str());
            u8g2.drawStr(0, 30, ("HOST: " + currHost).c_str());
            dispNetMode = "WIFI";
        } else if (showEth) {
            // Icon + lines for Ethernet
            drawEthCord(u8g2);
            String ipLine = "ETH : " + newDispIP;
            u8g2.drawStr(0, 18, ipLine.c_str());
            u8g2.drawStr(0, 30, ("HOST: " + currHost).c_str());
            dispNetMode = "ETH";
        } else if (showAp) {
            // NEW: AP instruction screen
            drawApStatus(u8g2);
            dispNetMode = "AP";
        } else {
            // Fallback: Neither up and AP not active
        const bool apOn = (WiFi.getMode() & WIFI_MODE_AP);
        const String ssid = apOn ? WiFi.softAPSSID() : String(WIFI_AP_NAME);

        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_7x13B_tr);

        // Line 1: "No Network"
        const char* l1 = "No Network";
        int16_t x1 = (128 - u8g2.getStrWidth(l1)) / 2;
        u8g2.drawStr(x1, 12, l1);

        // Line 2: "Connect to <ssid>"
        String l2 = "Connect to " + ssid;
        int16_t x2 = (128 - u8g2.getStrWidth(l2.c_str())) / 2;
        u8g2.drawStr(x2, 28, l2.c_str());

        u8g2.sendBuffer();
            dispNetMode = "NONE";
        }

        dispIP = newDispIP;
        dispHostName = currHost;
        dispRSSI = newDispRSSI;
        u8g2.sendBuffer();
    }

    xSemaphoreGive(displayMutex);
}

void c_OLED::UpdateRunningStatus()
{
    if (isRebooting)ESP.restart();
    if (!displayMutex || xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    static JsonDocument doc;
    doc.clear();
    JsonObject jsonStatus = doc.to<JsonObject>();
    FPPDiscovery.GetStatus(jsonStatus);

    if (jsonStatus.isNull() || !jsonStatus["current_sequence"].is<String>()) {
        xSemaphoreGive(displayMutex);
        return;
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 10, jsonStatus["current_sequence"].as<String>().c_str());
    u8g2.drawStr(0, 20, (jsonStatus["time_elapsed"].as<String>() + " / " + jsonStatus["time_remaining"].as<String>()).c_str());
    u8g2.sendBuffer();

    xSemaphoreGive(displayMutex);
}

void c_OLED::Update(bool forceUpdate)
{
    if (isRebooting)ESP.restart();
    if (!displayMutex || xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    unsigned long now = millis();
    if (currentPage == DisplayPage::NETWORK_INFO && (forceUpdate || now - lastNetworkUpdate >= NETWORK_UPDATE_INTERVAL_MS)) {
        xSemaphoreGive(displayMutex);
        UpdateNetworkInfo(forceUpdate);
        lastNetworkUpdate = now;
        return;
    }
    if (currentPage == DisplayPage::RUNNING_STATUS) {
        xSemaphoreGive(displayMutex);
        UpdateRunningStatus();
        return;
    }
    xSemaphoreGive(displayMutex);
}

void c_OLED::UpdateUploadStatus(const String &filename, int progress)
{
    if (isRebooting)ESP.restart();
    if (isRebooting || !displayMutex || xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    uploadFilename = filename;
    uploadProgress = progress;
    isUploading = true;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 12, uploadFilename.c_str());
    u8g2.drawStr(0, 28, ("Expand: " + String(progress) + "%").c_str());
    u8g2.drawFrame(0, 30, 128, 2);
    u8g2.drawBox(0, 30, map(progress, 0, 100, 0, 128), 2);
    u8g2.sendBuffer();

    xSemaphoreGive(displayMutex);
}

void c_OLED::ShowToast(const String &message, const String &title)
{
    if (isRebooting || !displayMutex || xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return;

    toastMessage    = message;
    isToastActive   = true;
    toastStartTime  = millis();

    u8g2.clearBuffer();

    uint8_t messageYOffset = 4;

    if (!title.isEmpty()) {
        u8g2.setFont(u8g2_font_6x10_tf);
        uint8_t titleWidth = u8g2.getStrWidth(title.c_str());
        u8g2.drawStr((128 - titleWidth) / 2, 10, title.c_str());
        messageYOffset = 14;
    }

    u8g2.setFont(u8g2_font_7x13B_tr);

    constexpr uint8_t MAX_TOAST_WIDTH = 120;
    constexpr uint8_t TOAST_X_ORIGIN  = 4;
    DrawCenteredWrappedText(u8g2, toastMessage, MAX_TOAST_WIDTH, TOAST_X_ORIGIN, messageYOffset);

    u8g2.setDrawColor(1);
    u8g2.drawRFrame(0, 0, 128, 32, 4);

    u8g2.sendBuffer();

    xSemaphoreGive(displayMutex);
}

void c_OLED::Poll()
{
    static unsigned long lastDebounceTime = 0;
    uint32_t notificationValue = 0;

    if (xTaskNotifyWait(0, ULONG_MAX, &notificationValue, 0) == pdTRUE) {
        if ((millis() - lastDebounceTime) > BUTTON_DEBOUNCE_DELAY_MS) {
            Flip();
            Update(true);
            lastDebounceTime = millis();
        }
    }

#ifdef BUTTON_GPIO1
    if (digitalRead(BUTTON_GPIO1) == LOW && (millis() - lastDebounceTime) > BUTTON_DEBOUNCE_DELAY_MS) {
        Flip();
        Update(true);
        lastDebounceTime = millis();
    }
#endif

    if (!isUploading && millis() - lastPageSwitchTime >= pageSwitchInterval) {
        currentPage = (FPPDiscovery.PlayingAfile() && currentPage == DisplayPage::NETWORK_INFO)
                        ? DisplayPage::RUNNING_STATUS : DisplayPage::NETWORK_INFO;
        lastPageSwitchTime = millis();
        Update(true);
    }

    if (isToastActive && millis() - toastStartTime >= TOAST_DURATION_MS) {
        isToastActive = false;
        Update(true);
    }
}

void c_OLED::Flip()
{
    if (isRebooting || !displayMutex || xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    flipState = !flipState;
    u8g2.setDisplayRotation(flipState ? U8G2_R2 : U8G2_R0);
    preferences.putBool("flipState", flipState);
    u8g2.updateDisplay();
    xSemaphoreGive(displayMutex);
}

void c_OLED::ShowRebootScreen()
{
    if (!displayMutex || xSemaphoreTake(displayMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    isRebooting = true;
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_profont15_tr);
    u8g2.drawStr(22, 12, "ESPixelStick");
    u8g2.setFont(u8g2_font_t0_22b_tr);
    u8g2.drawStr(15, 29, "REBOOTING");
    u8g2.sendBuffer();
    ESP.restart();
    xSemaphoreGive(displayMutex);
}

int c_OLED::getSignalStrength(int rssi)
{
    if (rssi > -50) return 4;
    if (rssi > -60) return 3;
    if (rssi > -70) return 2;
    if (rssi > -80) return 1;
    return 0;
}

void IRAM_ATTR c_OLED::buttonISR()
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (oledTaskHandle) {
        xTaskNotifyFromISR(oledTaskHandle, 1, eSetBits, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
    }
}

c_OLED OLED;
#endif
