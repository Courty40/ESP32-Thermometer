#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GC9A01A.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

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
    // fillTriangle manages its own SPI transaction. Wrapping it in startWrite
    // nests beginTransaction calls and can deadlock the ESP32 SPI mutex.
    for (int i = 0; i < GAUGE_SEGMENTS; ++i) {
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

void showStatus(const char *status) {
    if (previousStatus == status) return;
    previousStatus = status;
    display.fillRect(15, 85, 210, 100, GC9A01A_BLACK);
    centeredText("--.-", 95, 5, GC9A01A_DARKGREY);
    centeredText(status, 165, 1, GC9A01A_YELLOW);
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
    snprintf(reading, sizeof(reading), "%.1f", celsius);
    if (strlen(reading) > 8) {
        showStatus("Temperature out of range");
        return;
    }
    previousStatus = "";
    display.fillRect(15, 85, 210, 100, GC9A01A_BLACK);
    centeredText(reading, 95, strlen(reading) > 6 ? 3 : (strlen(reading) > 5 ? 4 : 5), GC9A01A_CYAN);
    centeredText("Celsius", 145, 2, GC9A01A_WHITE);
    centeredText("Connected", 175, 1, GC9A01A_GREEN);
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
    Serial.println("Display initialized; drawing temperature gauge.");
    drawGauge(0, false);
    centeredText("THERMOMETER", 45, 2, GC9A01A_WHITE);
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
        showStatus("Set up include/thermometer_config.h");
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
        if (!wasConnected && String(HA_BASE_URL).startsWith("https://")) {
            configTime(0, 0, NTP_SERVER);
        }
        wasConnected = true;
        lastPoll = now;
        fetchTemperature();
    }
    delay(10);
}

