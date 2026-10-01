#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GC9A01A.h>
#include <Fonts/FreeSans24pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include "temperature_history.h"

#if __has_include("thermometer_config.h")
#include "thermometer_config.h"
#else
#include "thermometer_config.example.h"
#endif

constexpr uint8_t LCD_DC = 8, LCD_CS = 9, LCD_SCK = 10;
constexpr uint8_t LCD_MOSI = 11, LCD_RST = 12, LCD_BL = 40;
constexpr uint32_t WIFI_RETRY_MS = 15000;
Adafruit_GC9A01A display(&SPI, LCD_DC, LCD_CS, LCD_RST);
uint32_t lastWifiAttempt = 0, lastPoll = 0;
bool wasConnected = false;
String previousStatus;
String previousReading;
bool welcomeVisible = true;
constexpr uint32_t WELCOME_MIN_MS = 2500;
uint32_t welcomeStarted = 0;
GFXcanvas16 centreCanvas(168, 104);
int previousGaugeFill = -1;
TemperatureHistory history;
TemperatureHistory::Trend previousTrend = TemperatureHistory::Trend::Unknown;

// Clockwise 270-degree gauge: -10 C at bottom-left, +40 C at bottom-right.
constexpr float GAUGE_MIN_C = -10.0f, GAUGE_MAX_C = 40.0f;
constexpr int GAUGE_SEGMENTS = 270;

uint16_t gaugeColor(float fraction) {
    // Blue -> cyan -> green -> yellow -> red.
    const float position = constrain(fraction, 0.0f, 1.0f) * 4.0f;
    const int band = min(static_cast<int>(position), 3);
    const float blend = position - band;
    const uint8_t colors[5][3] = {
        {0, 0, 255}, {0, 255, 255}, {0, 255, 0}, {255, 255, 0}, {255, 0, 0}
    };
    return display.color565(
        colors[band][0] + (colors[band + 1][0] - colors[band][0]) * blend,
        colors[band][1] + (colors[band + 1][1] - colors[band][1]) * blend,
        colors[band][2] + (colors[band + 1][2] - colors[band][2]) * blend);
}

void drawGauge(float celsius, bool valid) {
    const float fraction = constrain((celsius - GAUGE_MIN_C) /
                                     (GAUGE_MAX_C - GAUGE_MIN_C), 0.0f, 1.0f);
    const int filled = valid ? static_cast<int>(roundf(fraction * GAUGE_SEGMENTS)) : 0;
    if (filled == previousGaugeFill) return;
    const int first = previousGaugeFill < 0 ? 0 : max(0, min(filled, previousGaugeFill) - 1);
    const int last = previousGaugeFill < 0 ? GAUGE_SEGMENTS : min(GAUGE_SEGMENTS, max(filled, previousGaugeFill) + 1);
    // fillTriangle manages its own SPI transaction. Wrapping it in startWrite
    // nests beginTransaction calls and can deadlock the ESP32 SPI mutex.
    for (int i = first; i < last; ++i) {
        const float a = (135.0f + i) * PI / 180.0f;
        const float b = (136.1f + i) * PI / 180.0f; // Slight overlap avoids gaps.
        const int16_t x0 = lroundf(120 + 108 * cosf(a));
        const int16_t y0 = lroundf(120 + 108 * sinf(a));
        const int16_t x1 = lroundf(120 + 117 * cosf(a));
        const int16_t y1 = lroundf(120 + 117 * sinf(a));
        const int16_t x2 = lroundf(120 + 117 * cosf(b));
        const int16_t y2 = lroundf(120 + 117 * sinf(b));
        const int16_t x3 = lroundf(120 + 108 * cosf(b));
        const int16_t y3 = lroundf(120 + 108 * sinf(b));
        const uint16_t color = i < filled ? gaugeColor(i / float(GAUGE_SEGMENTS - 1))
                                         : GC9A01A_DARKGREY;
        display.fillTriangle(x0, y0, x1, y1, x2, y2, color);
        display.fillTriangle(x0, y0, x2, y2, x3, y3, color);
    }
    previousGaugeFill = filled;
}

void centeredText(const char *text, int16_t y, uint8_t size, uint16_t color) {
    display.setTextSize(size);
    display.setTextColor(color, GC9A01A_BLACK);
    int16_t x1, y1;
    uint16_t width, height;
    display.getTextBounds(text, 0, y, &x1, &y1, &width, &height);
    display.setCursor((240 - static_cast<int16_t>(width)) / 2, y);
    display.print(text);
}

void canvasText(const char *text, int16_t y, uint8_t size, uint16_t color) {
    centreCanvas.setFont(nullptr);
    centreCanvas.setTextSize(size);
    centreCanvas.setTextColor(color);
    centreCanvas.setTextWrap(false);
    const int width = strlen(text) * 6 * size;
    centreCanvas.setCursor((centreCanvas.width() - width) / 2, y);
    centreCanvas.print(text);
}

void drawHistory(uint32_t now) {
    constexpr int LEFT = 6, RIGHT = 161, TOP = 80, BOTTOM = 102;
    centreCanvas.setTextSize(1);
    centreCanvas.setTextColor(GC9A01A_DARKGREY);
    centreCanvas.setCursor(LEFT, 69);
    centreCanvas.print("-1h");
    centreCanvas.setCursor(RIGHT - 18, 69);
    centreCanvas.print("now");
    centreCanvas.drawFastHLine(LEFT, BOTTOM, RIGHT - LEFT + 1, GC9A01A_DARKGREY);
    float low = INFINITY, high = -INFINITY;
    for (uint8_t i = 0; i < history.count(); ++i) {
        const auto &sample = history.at(i);
        if (uint32_t(now - sample.time) > TemperatureHistory::WINDOW_MS) continue;
        low = fminf(low, sample.celsius);
        high = fmaxf(high, sample.celsius);
    }
    if (!isfinite(low)) return;
    const float middle = (low + high) * 0.5f;
    const float span = fmaxf(2.0f, high - low + 0.4f);
    low = middle - span * 0.5f;
    bool havePrevious = false;
    int previousX = 0, previousY = 0;
    uint32_t previousTime = 0;
    for (uint8_t i = 0; i < history.count(); ++i) {
        const auto &sample = history.at(i);
        const uint32_t age = now - sample.time;
        if (age > TemperatureHistory::WINDOW_MS) continue;
        const int x = RIGHT - lroundf(float(age) / TemperatureHistory::WINDOW_MS * (RIGHT - LEFT));
        const int y = BOTTOM - 1 - lroundf((sample.celsius - low) / span * (BOTTOM - TOP - 1));
        if (havePrevious && uint32_t(sample.time - previousTime) <= TemperatureHistory::MAX_GAP_MS)
            centreCanvas.drawLine(previousX, previousY, x, y, GC9A01A_CYAN);
        centreCanvas.drawPixel(x, y, GC9A01A_CYAN);
        havePrevious = true;
        previousX = x; previousY = y; previousTime = sample.time;
    }
}

void drawTrend(TemperatureHistory::Trend trend) {
    using Trend = TemperatureHistory::Trend;
    const char *label = trend == Trend::Rising ? "Warming" :
                        trend == Trend::Falling ? "Cooling" :
                        trend == Trend::Steady ? "Steady" : "Learning...";
    const uint16_t color = trend == Trend::Rising ? GC9A01A_ORANGE :
                           trend == Trend::Falling ? GC9A01A_CYAN : GC9A01A_LIGHTGREY;
    canvasText(label, 54, 1, color);
    if (trend == Trend::Unknown) return;
    const int x = 42;
    if (trend == Trend::Steady) {
        centreCanvas.drawLine(x - 6, 57, x + 5, 57, color);
        centreCanvas.fillTriangle(x + 7, 57, x + 3, 54, x + 3, 60, color);
    } else {
        const int tip = trend == Trend::Rising ? 51 : 62;
        const int tail = trend == Trend::Rising ? 62 : 51;
        centreCanvas.drawLine(x, tail, x, tip, color);
        centreCanvas.fillTriangle(x, tip, x - 4, (tip + tail) / 2, x + 4, (tip + tail) / 2, color);
    }
}

void renderCentre(const char *reading, const char *status, bool valid,
                  TemperatureHistory::Trend trend = TemperatureHistory::Trend::Unknown) {
    // Compose in RAM, then transfer once: no visible clear/redraw cycle.
    centreCanvas.fillScreen(GC9A01A_BLACK);
    if (valid) {
        centreCanvas.setTextSize(1);
        centreCanvas.setTextColor(GC9A01A_CYAN);
        centreCanvas.setFont(&FreeSans24pt7b);
        int16_t x1, y1;
        uint16_t width, height;
        centreCanvas.getTextBounds(reading, 0, 0, &x1, &y1, &width, &height);
        if (width > 164) {
            centreCanvas.setFont(&FreeSans18pt7b);
            centreCanvas.getTextBounds(reading, 0, 0, &x1, &y1, &width, &height);
        }
        centreCanvas.setCursor((168 - static_cast<int>(width)) / 2 - x1, 3 - y1);
        centreCanvas.print(reading);
        canvasText("Celsius", 44, 1, GC9A01A_WHITE);
        drawTrend(trend);
        drawHistory(millis());
    } else {
        canvasText(reading, 15, strlen(reading) > 5 ? 3 : 5, GC9A01A_WHITE);
        canvasText(status, 90, 1, GC9A01A_YELLOW);
    }
    display.drawRGBBitmap(36, 80, centreCanvas.getBuffer(), 168, 104);
}

void showStatus(const char *status) {
    if (previousStatus == status) return;
    previousStatus = status;
    previousReading = "";
    renderCentre(welcomeVisible ? "Welcome" : "--.-", status, false);
    drawGauge(0, false);
    Serial.println(status);
}
bool configured() {
    return strlen(WIFI_SSID) && strlen(HA_BASE_URL) && strlen(HA_TOKEN) &&
           strlen(HA_ENTITY_ID) && HA_POLL_INTERVAL_MS >= 1000;
}

void fetchTemperature() {
    String base(HA_BASE_URL);
    while (base.endsWith("/")) base.remove(base.length() - 1);
    const String url = base + "/api/states/" + HA_ENTITY_ID;
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    HTTPClient http;
    http.useHTTP10(true); // Stream JSON without chunked transfer encoding.
    http.setConnectTimeout(4000);
    http.setTimeout(4000);
    // Never follow redirects carrying a Home Assistant access token.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (base.startsWith("https://")) {
        if (!strlen(HA_ROOT_CA)) {
            showStatus("HTTPS needs CA cert");
            return;
        }
        if (time(nullptr) < 1700000000) {
            showStatus("Waiting for HTTPS clock");
            return;
        }
        secureClient.setCACert(HA_ROOT_CA);
        if (!http.begin(secureClient, url)) {
            showStatus("Invalid HA URL");
            return;
        }
    } else if (base.startsWith("http://")) {
        if (!http.begin(plainClient, url)) {
            showStatus("Invalid HA URL");
            return;
        }
    } else {
        showStatus("Use http:// or https://");
        return;
    }
    http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
    http.addHeader("Accept", "application/json");
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        if (code == 401 || code == 403) showStatus("Check HA access token");
        else if (code == 404) showStatus("Entity not found");
        else showStatus("Home Assistant offline");
        Serial.printf("HA request status: %d\n", code);
        return;
    }
    // Keep only the fields we need, even if the entity has large attributes.
    StaticJsonDocument<192> filter;
    filter["state"] = true;
    filter["attributes"]["unit_of_measurement"] = true;
    StaticJsonDocument<512> document;
    const DeserializationError error = deserializeJson(
        document, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (error) {
        showStatus("Invalid HA response");
        return;
    }
    const char *state = document["state"] | "";
    char *end = nullptr;
    const float value = strtof(state, &end);
    if (end == state) {
        showStatus("Temperature unavailable");
        return;
    }
    while (*end && isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end || !isfinite(value)) {
        showStatus("Invalid temperature");
        return;
    }
    const char *unit = document["attributes"]["unit_of_measurement"] | "";
    float celsius = value;
    if (!strcmp(unit, "\xC2\xB0" "F")) celsius = (value - 32.0f) / 1.8f;
    else if (!strcmp(unit, "K")) celsius = value - 273.15f;
    else if (strcmp(unit, "\xC2\xB0" "C")) {
        showStatus("Check temperature unit");
        return;
    }
    char reading[24];
    snprintf(reading, sizeof(reading), "%.2f", celsius);
    if (strlen(reading) > 8) {
        showStatus("Temperature out of range");
        return;
    }
    welcomeVisible = false;
    const uint32_t sampleTime = millis();
    const bool historyChanged = history.record(celsius, sampleTime);
    const auto trend = history.trend(celsius, sampleTime);
    if (previousReading != reading || previousStatus != "Connected" || historyChanged || trend != previousTrend) {
        renderCentre(reading, "Connected", true, trend);
        previousReading = reading;
        previousStatus = "Connected";
        previousTrend = trend;
    }
    drawGauge(celsius, true);
    Serial.printf("Temperature: %.2f C\n", celsius);
}

void setup() {
    Serial.begin(115200);
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, LOW);
    SPI.begin(LCD_SCK, -1, LCD_MOSI, LCD_CS);
    display.begin(40000000);
    display.setRotation(0);
    display.invertDisplay(true);
    display.setTextWrap(false);
    display.fillScreen(GC9A01A_BLACK);
    digitalWrite(LCD_BL, HIGH);
    renderCentre("Welcome", "Starting...", false);
    welcomeStarted = millis();
    Serial.println("Display initialized; drawing temperature gauge.");
    drawGauge(0, false);
    centeredText("Home Assistant", 200, 1, GC9A01A_DARKGREY);
    display.setTextSize(1);
    display.setTextColor(GC9A01A_BLUE);
    display.setCursor(56, 210);
    display.print("-10");
    display.setTextColor(GC9A01A_RED);
    display.setCursor(160, 210);
    display.print("+40");
    digitalWrite(LCD_BL, HIGH);
    if (!configured()) {
        showStatus("Set up configuration");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiAttempt = millis();
    showStatus("Connecting Wi-Fi...");
}

void loop() {
    if (!configured()) {
        delay(100);
        return;
    }
    const uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        wasConnected = false;
        showStatus("Connecting Wi-Fi...");
        if (now - lastWifiAttempt >= WIFI_RETRY_MS) {
            lastWifiAttempt = now;
            WiFi.reconnect();
        }
    } else if (!wasConnected || now - lastPoll >= HA_POLL_INTERVAL_MS) {
        // Wi-Fi connects immediately, but keep Welcome readable before the first poll.
        if (welcomeVisible && now - welcomeStarted < WELCOME_MIN_MS) {
            showStatus("Wi-Fi connected");
            delay(10);
            return;
        }
        if (!wasConnected && String(HA_BASE_URL).startsWith("https://")) {
            configTime(0, 0, NTP_SERVER);
        }
        if (!wasConnected) showStatus("Connecting to HA...");
        wasConnected = true;
        lastPoll = now;
        fetchTemperature();
    }
    delay(10);
}


