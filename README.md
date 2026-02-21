<img src="example.png" alt="example" width="350" />

# ESP32 UPS Serial to SNMP

This project runs on an **ESP32-C3** and bridges an APC UPS serial interface to a lightweight SNMP agent over Wi‑Fi.

## Hardware required
- **MAX3232/SP3232 (or equivalent) RS232 To TTL Converter Module is required.**
	The UPS serial port uses RS‑232 voltage levels; the ESP32 UART is 3.3V TTL. Do not connect the UPS RS‑232 lines directly to the ESP32.

- **DB9 adapter/cable (as needed).**
	Depending on your UPS serial port gender and pinout, you may need a DB9 male↔female adapter and either a **null‑modem** or **straight‑through** serial adapter/cable. (The author is using a **null‑modem** adapter.)

### 24/7 stability note
This is typically a **7×24** kind of device. For better long‑term stability, consider using:
- A higher quality ESP32-C3 dev board / module with a stable 3.3V supply.
- An RS‑232 interface with **isolated power and isolated signal** (or an isolated DC‑DC + isolated transceiver) to reduce ground loops and noise.

## What it does
- Reads UPS telemetry over UART (default: `2400` baud).
- Starts a Wi‑Fi station client.
- Exposes UPS values via SNMP (`UDP/161`, community string configurable).

## Quick configuration
Wi‑Fi SSID/password are compiled in via build flags.

Edit `platformio.ini` under `[env:esp32-c3-devkitm-1]` → `build_flags`:
- `-D UPS_WIFI_STA_SSID=\"YOUR_WIFI_SSID\"`
- `-D UPS_WIFI_STA_PASSWORD=\"YOUR_WIFI_PASSWORD\"`
- `-D UPS_SNMP_COMMUNITY=\"public\"`

If `UPS_WIFI_STA_SSID` is empty/invalid, the firmware will skip starting Wi‑Fi.

Optional UART overrides (also via build flags):
- `UPS_UART_TX_GPIO` (default `0`)
- `UPS_UART_RX_GPIO` (default `1`)
- `UPS_UART_BAUDRATE` (default `2400`)

UART selection note: some ESP32-C3 dev boards use a hardware UART for bootloader flashing, firmware download, and/or serial logging. If upload/monitor stops working or you see mixed debug output, keep the UPS on a different UART and choose non-conflicting TX/RX GPIOs.

## Build and flash
From the project root:

```bash
platformio run
platformio run -t upload
platformio device monitor
```

Default PlatformIO environment: `esp32-c3-devkitm-1`.

## License
See `LICENSE`.
