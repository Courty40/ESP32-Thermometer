# Waveshare Home Assistant thermometer

PlatformIO / Arduino firmware for the non-touch ESP32-S3-LCD-1.28. Connects to 2.4 GHz Wi-Fi and reads one temperature sensor through Home Assistant's REST API every 10 seconds. Shows Celsius with one decimal place; Fahrenheit and Kelvin source units are converted. No DS18B20 wiring is needed.

## Setup

1. Copy `include/thermometer_config.example.h` to `include/thermometer_config.h`.
2. In `thermometer_config.h`, enter your Wi-Fi SSID and password.
3. The configured Home Assistant URL is `http://homeassistant.local:8123`. Adjust the port or scheme if your instance uses something different. A reserved DHCP address is useful.
4. In Home Assistant, open **Developer Tools → States**, find the temperature sensor, and copy its entity ID into `HA_ENTITY_ID`. This version reads the entity's numeric state (not a climate/weather entity's temperature attribute).
5. Open your Home Assistant user profile's **Security** tab and create a **Long-Lived Access Token**. Paste it into `HA_TOKEN` locally. Do not paste it into chat. The private `thermometer_config.h` is ignored by Git; credentials are still compiled into the firmware.
6. Open this folder in VS Code with PlatformIO. Click **Build**, connect the board using a USB data cable, then click **Upload**.
7. Open **Serial Monitor** at 115200 baud for readings and connection status. Tokens and Wi-Fi passwords are not logged.

The REST API uses authenticated `GET /api/states/<entity_id>` requests. It only reads the selected entity. Local HTTP sends the token without transport encryption, so use it only on your trusted LAN. HTTPS is also supported: set an HTTPS URL, supply its root CA certificate in `HA_ROOT_CA`, and set `NTP_SERVER` to a reachable time server. HTTPS waits for clock synchronization and validates the certificate; certificates must match the hostname/IP in the URL.

## Behaviour and troubleshooting

A welcome screen appears as soon as the LCD is initialized and remains while Wi-Fi and Home Assistant connect. The centre panel is composed in memory before transfer to reduce flicker. Unchanged readings are not redrawn, and only changed ring segments are updated.

The outer gauge fills clockwise from -10 °C at bottom-left to +40 °C at bottom-right, with a blue → cyan → green → yellow → red gradient. The remaining arc is grey. Values outside the range clamp the gauge to empty/full while the numeric reading remains unchanged. Connection errors clear the coloured arc.

- The firmware retries Wi-Fi connections and polls Home Assistant automatically.
- Missing Wi-Fi, request failures, `unknown`/`unavailable` states, invalid numbers, and unsupported/missing units clear the displayed reading and show a status message.
- `Check HA access token`: token rejected (HTTP 401/403).
- `Entity not found`: check the entity ID (HTTP 404).
- `Check temperature unit`: the entity must report °C, °F, or K in `unit_of_measurement`.
- `Home Assistant offline`: check address, port, network access, and Serial Monitor's HTTP status code.
- `Set up include/thermometer_config.h`: required configuration is empty. Rebuild/upload after changing it.

The displayed value is Home Assistant's current stored state. Polling does not force the upstream sensor to take a new measurement, and a successful response does not prove that sensor has reported recently.

Change `HA_POLL_INTERVAL_MS` to adjust polling (minimum 1000 ms). HTTP connection/read timeouts are four seconds each. Display rotation is controlled by `display.setRotation(0)` in `src/main.cpp`.

CLI equivalents: `pio run`, `pio run -t upload`, `pio device monitor`. If necessary, add your actual `upload_port = COMx` and `monitor_port = COMx` to `platformio.ini`. For manual download mode, hold BOOT, press/release RESET, release BOOT, then upload. Press RESET after uploading if needed.

The display pins are DC=8, CS=9, SCK=10, MOSI=11, RESET=12, and backlight=40. The generic ESP32-S3 profile is configured for 16 MB flash; the default partition layout is sufficient for this app. PSRAM is not needed. USB uses the board's CH343 USB-to-UART bridge.

## Sources

- [Waveshare board documentation](https://docs.waveshare.com/ESP32-S3-LCD-1.28)
- [Home Assistant REST API](https://developers.home-assistant.io/docs/api/rest/)

Live Wi-Fi and Home Assistant operation needs verification on your board with your configuration.

