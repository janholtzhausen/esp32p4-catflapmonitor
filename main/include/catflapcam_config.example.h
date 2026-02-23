#pragma once

/*
 * Copy this file to catflapcam_config.h and set your credentials.
 * catflapcam_config.h is gitignored and should stay local.
 */
#define CATFLAPCAM_WIFI_SSID "replace-with-your-ssid"
#define CATFLAPCAM_WIFI_PASSWORD "replace-with-your-password"
#define CATFLAPCAM_WIFI_MAX_RETRY 6
#define CATFLAPCAM_OTA_PASSWORD "replace-with-ota-password"

/*
 * SD snapshot storage (FATFS on SDMMC).
 * Note: ESP-IDF does not provide F2FS support on-device.
 */
#define CATFLAPCAM_SDCARD_ENABLE 1
#define CATFLAPCAM_SDCARD_MOUNT_POINT "/sdcard"
#define CATFLAPCAM_SDCARD_SLOT 0
#define CATFLAPCAM_SDCARD_BUS_WIDTH 4
#define CATFLAPCAM_SDCARD_MAX_FREQ_KHZ 20000
#define CATFLAPCAM_SDCARD_FORMAT_IF_MOUNT_FAILED 0
#define CATFLAPCAM_SNAPSHOT_MAX_FILES 20000
#define CATFLAPCAM_SDCARD_USE_INTERNAL_LDO 1
#define CATFLAPCAM_SDCARD_LDO_ID 4
#define CATFLAPCAM_SNAPSHOT_WIDTH 224
#define CATFLAPCAM_SNAPSHOT_HEIGHT 224
#define CATFLAPCAM_SNAPSHOT_JPEG_QUALITY 100
