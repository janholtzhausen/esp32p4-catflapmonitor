# catflapcam

`catflapcam` is an ESP-IDF firmware project for ESP32-P4 camera systems with:
- Wi-Fi web streaming
- local SD-card snapshot capture
- ring-buffered snapshot retention
- OTA firmware updates with password auth
- optional ultrasonic-triggered snapshot capture

The design goal is reliable edge capture first: snapshots are saved locally and served directly from the device.

## Target Platform

- MCU: ESP32-P4
- Camera stack: `esp_video` + V4L2-style capture path
- Network: Wi-Fi (no Ethernet path)
- Storage: SD card via SDMMC + FATFS (`/sdcard/snapshots`)
- Web server: `esp_http_server`

## Core Features

- MJPEG stream endpoint per camera source (`/stream` on source ports)
- Manual snapshot trigger from web UI (`/api/capture_image?source=<idx>`)
- Snapshot storage ring (`CATFLAPCAM_SNAPSHOT_MAX_FILES`)
- Timestamped snapshot filenames
- Snapshot gallery page with delete support (`/snapshots`)
- OTA endpoint with constant-time password check (`/api/ota`)
- mDNS and NetBIOS name advertisement

## Project Layout

- `main/main.c`: system bootstrap (NVS, netif/event loop, Wi-Fi, video, storage, HTTP, ultrasonic)
- `main/catflapcam_webcam.c`: camera capture, snapshot pipeline, JPEG encoding
- `main/catflapcam_storage.c`: SD mount, ring retention, list/resolve/delete snapshot files
- `main/catflapcam_http_server.c`: static UI, stream, snapshot, and OTA routes
- `main/catflapcam_ultrasonic.c`: HC-SR04 trigger task (optional)
- `main/include/catflapcam_config.example.h`: local runtime configuration template
- `partitions.csv`: OTA partition layout (`ota_0` / `ota_1`)

## Configuration

Create a local config header:

```bash
cp main/include/catflapcam_config.example.h main/include/catflapcam_config.h
```

`catflapcam_config.h` is local-only and should not be committed.

Required fields:

```c
#define CATFLAPCAM_WIFI_SSID "your-ssid"
#define CATFLAPCAM_WIFI_PASSWORD "your-password"
#define CATFLAPCAM_OTA_PASSWORD "your-ota-password"
```

Key snapshot/storage tuning:

```c
#define CATFLAPCAM_SNAPSHOT_MAX_FILES 20000
#define CATFLAPCAM_SNAPSHOT_WIDTH 224
#define CATFLAPCAM_SNAPSHOT_HEIGHT 224
#define CATFLAPCAM_SNAPSHOT_JPEG_QUALITY 100
#define CATFLAPCAM_SDCARD_SLOT 0
#define CATFLAPCAM_SDCARD_BUS_WIDTH 4
#define CATFLAPCAM_SDCARD_MAX_FREQ_KHZ 20000
```

## Build and Flash

```bash
idf.py build
idf.py -p COM4 flash
idf.py -p COM4 monitor
```

If serial monitor locks the port, close the stale monitor process before flashing.

## HTTP API

- `GET /`  
  Main web UI.

- `GET /api/get_camera_info`  
  Camera metadata and stream source info.

- `GET /api/capture_image?source=<index>`  
  Captures one frame and stores it as a snapshot on SD.

- `GET /api/snapshots?limit=<n>`  
  Returns JSON list of latest snapshots.

- `GET /snapshots`  
  Snapshot gallery page.

- `GET /snapshots/<filename>`  
  Serves one snapshot JPEG.

- `DELETE /api/snapshots/<filename>`  
  Deletes a snapshot from SD.

- `POST /api/ota`  
  OTA update endpoint. Requires `X-OTA-Password` header.

## OTA Update

```bash
curl -X POST \
  -H "X-OTA-Password: your-ota-password" \
  --data-binary "@build/catflapcam.bin" \
  http://<device-ip>/api/ota
```

Expected responses:
- `401 Unauthorized`: missing/invalid password
- `OK`: image accepted; device reboots into new OTA slot

## Storage Behavior

- Snapshots are stored in `/sdcard/snapshots`.
- Filenames include a monotonic sequence + local timestamp for easier inspection.
- Retention is a ring by file count (`CATFLAPCAM_SNAPSHOT_MAX_FILES`).
- Oldest snapshots are evicted automatically when the limit is reached.

Note: this firmware uses FATFS for SD cards in ESP-IDF. F2FS/LittleFS are not used for the SD snapshot path.

## SD Card Access Details

This project uses ESP-IDF SDMMC host mode + FATFS mount (`esp_vfs_fat_sdmmc_mount`) with explicit reliability guards:

- Host configuration:
  - `SDMMC_HOST_DEFAULT()` is used
  - Slot selected by `CATFLAPCAM_SDCARD_SLOT`
  - Bus width selected by `CATFLAPCAM_SDCARD_BUS_WIDTH`
  - SD clock capped by `CATFLAPCAM_SDCARD_MAX_FREQ_KHZ`

- Power and signal stability:
  - Internal pull-ups are enabled (`SDMMC_SLOT_FLAG_INTERNAL_PULLUP`)
  - Optional on-chip LDO power control is supported via:
    - `CATFLAPCAM_SDCARD_USE_INTERNAL_LDO`
    - `CATFLAPCAM_SDCARD_LDO_ID`
  - This is useful on boards where SD rail sequencing causes init timeouts.

- Mount strategy:
  - First attempts configured width (typically 4-bit)
  - If mount fails and width > 1, firmware retries automatically in 1-bit mode
  - `CATFLAPCAM_SDCARD_FORMAT_IF_MOUNT_FAILED` controls auto-format behavior
  - Mount point is `CATFLAPCAM_SDCARD_MOUNT_POINT` (default `/sdcard`)

- Runtime storage layout:
  - Snapshot directory: `/sdcard/snapshots`
  - File naming: `snap-<seq>-<yyyymmdd-hhmmss>.jpg`
  - Sequence remains monotonic and is used for deterministic oldest-file eviction
  - Ring retention is implemented in firmware, not filesystem-level

- Why ring-by-file-count is used:
  - Predictable retention behavior
  - Bounded directory growth for embedded list/scan operations
  - Better control of RAM/latency during snapshot indexing and gallery listing

- Common pitfall (ESP32-P4 + hosted Wi-Fi systems):
  - Wi-Fi hosted transport and SD card can contend for SDMMC resources if slot wiring/config is incorrect.
  - Keep SD slot selection aligned with board design and hosted transport configuration.

## Reliability Notes

- Wi-Fi-first runtime (Ethernet path removed).
- NVS recovery handles `ESP_ERR_NVS_NO_FREE_PAGES` and `ESP_ERR_NVS_NEW_VERSION_FOUND`.
- Startup logs include reset reason.
- `CONFIG_UART_ISR_IN_IRAM=y` is recommended for robust logging/flash concurrency on ESP32 targets.
- Capture path prioritizes snapshot operations over stream when required.

## Troubleshooting

- `spi_flash: Detected size ... larger than the size in the binary image header ...`  
  Align flash-size config and image header settings to your board.

- SD mount failures/timeouts  
  Check `CATFLAPCAM_SDCARD_SLOT`, bus width, clock, pull-ups, and board power/LDO setup.

- OTA rejected  
  Verify `CATFLAPCAM_OTA_PASSWORD` and request header `X-OTA-Password`.

- Snapshot gallery issues  
  Confirm SD storage is mounted and `/api/snapshots` returns valid JSON.

## TODO

- Teachable Machine / cloud workflow:
  - Add optional upload endpoint mode for pushing selected snapshots to a Teachable Machine-compatible collector.
  - Keep local SD snapshot as source-of-truth, with upload as a secondary async task.
  - Add retry/backoff queue and upload status metadata per snapshot.

- On-device inference (tiny tensor / YOLO):
  - Add lightweight on-device model pipeline for immediate cat identification at capture time.
  - Prioritize "known cat" vs "unknown/intruder cat" classification with low-latency execution.
  - Add configurable confidence thresholds and debounce logic to reduce false alerts.

- MQTT integration:
  - Publish real-time events for:
    - `catflapcam/cat/known`
    - `catflapcam/cat/intruder`
    - `catflapcam/system/error`
  - Include snapshot filename, confidence, timestamp, and source index in message payload.
  - Add broker auth/TLS configuration and offline queueing for intermittent Wi-Fi.
