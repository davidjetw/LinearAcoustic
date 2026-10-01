# ESP32-C6 firmware 26.09.30+4

OTA download timeout fix. Compiled for ESP32-C6, 4 MB flash, min_spiffs (1.9 MB OTA slots).

- File: `LAV60II.bin`
- Version: `26.09.30+4`
- Size: 1,447,632 bytes
- SHA-256: `fb72d252f1d51103f9bc5c617c4d008d2fb64b783cb9d5ab90f5d099de3513b9`

Keeps Wi-Fi awake during download, allows ten minutes total with a 30-second inactivity limit, and shows download statistics. Original 100 ms power-off relay sequence is preserved.

Devices running +2/+3 still use their original three-minute download limit until this firmware is installed. If online installation times out, upload the application bin manually through /ota or install via USB. Actual download speed requires device testing.
