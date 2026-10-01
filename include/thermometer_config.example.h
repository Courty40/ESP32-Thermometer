#pragma once

// Copy this file to thermometer_config.h and enter your details locally.
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
constexpr char HA_BASE_URL[] = "http://homeassistant.local:8123";
constexpr char HA_TOKEN[] = ""; // Home Assistant long-lived access token
constexpr char HA_ENTITY_ID[] = "sensor.example_temperature"; // e.g. sensor.living_room_temperature
constexpr unsigned long HA_POLL_INTERVAL_MS = 10000;

// For HTTPS, paste the server's root CA certificate here as a PEM raw string.
// HTTP on your trusted local network does not use this setting.
constexpr char HA_ROOT_CA[] = "";
constexpr char NTP_SERVER[] = "pool.ntp.org"; // Needed for HTTPS certificate dates


