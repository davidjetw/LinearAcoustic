#pragma once

// Release identity: change only when publishing a new firmware release.
#define AUDIO_FW_VERSION "26.09.30+1"
#define AUDIO_OTA_PRODUCT "linear-acoustic-lav60ii"
#define AUDIO_OTA_BOARD "esp32c6"
#define AUDIO_OTA_MANIFEST_URL "https://raw.githubusercontent.com/davidjetw/LinearAcoustic/main/OTA/ESP32C6/version.json"
#define AUDIO_OTA_DOWNLOAD_PREFIX "https://raw.githubusercontent.com/davidjetw/LinearAcoustic/"

// Each device generates its own random password in NVS on first boot.
// USB serial command: OTA PASSWORD (115200 baud, newline enabled).
// Never embed a shared OTA password into a publicly downloadable firmware.
#define AUDIO_OTA_USER "admin"
