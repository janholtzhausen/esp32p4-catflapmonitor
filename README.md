# catflapcam Bring-Up and Debug Writeup

This document explains how the project reached a stable state with all three working together:
- SD card snapshot storage
- Serial monitor/debug workflow on COM4
- Wi-Fi networking (ESP-Hosted path)

It is written as a practical runbook so future changes can be debugged quickly.

## 1) Platform and Reality Checks

Target hardware path used in this project:
- Main MCU: ESP32-P4
- Wi-Fi link: ESP-Hosted over SDIO to C6 side
- Local storage: microSD card (currently 64GB)
- Serial port: COM4

Important practical constraints observed:
- Wi-Fi and SD card both involve SDMMC resources, so slot selection/order matters.
- If COM4 is already owned by another monitor process, flashing/monitoring fails until that process is closed.
- ESP-IDF on this project uses FATFS for SD storage; on-device F2FS formatting/mounting is not provided by ESP-IDF components used here.

## 2) Why SD Card Initially Failed

Typical failure looked like SD init/mount timeouts (e.g. `ESP_ERR_TIMEOUT`).
Root causes were a combination of:
- SDIO resource overlap assumptions between hosted Wi-Fi and SD card
- Initialization order and slot selection
- Power/voltage behavior on this board path

What was validated during debugging:
1. Hosted Wi-Fi path uses SDIO resources that can conflict with bad slot choice.
2. Probing the wrong slot at the wrong time can destabilize hosted startup.
3. A stable configuration is:
   - keep hosted path where expected
   - mount user SD card on the compatible slot for this board config
   - use on-chip LDO support when required by board wiring/power behavior

## 3) SD Card Working Configuration

The storage implementation now:
- Mounts SD card at `/sdcard`
- Uses `FATFS` + SDMMC host path
- Stores snapshots under `/sdcard/snapshots`
- Maintains a ring policy (`CATFLAPCAM_SNAPSHOT_MAX_FILES`) so oldest files are removed first when full

Relevant config flags in local config:
- `CATFLAPCAM_SDCARD_ENABLE`
- `CATFLAPCAM_SDCARD_SLOT`
- `CATFLAPCAM_SDCARD_BUS_WIDTH`
- `CATFLAPCAM_SDCARD_MAX_FREQ_KHZ`
- `CATFLAPCAM_SDCARD_FORMAT_IF_MOUNT_FAILED`
- `CATFLAPCAM_SNAPSHOT_MAX_FILES`
- `CATFLAPCAM_SDCARD_USE_INTERNAL_LDO`
- `CATFLAPCAM_SDCARD_LDO_ID`

Notes:
- Start conservative on frequency if card behavior is unstable, then raise gradually.
- Keep bus width aligned with actual board wiring (1-bit vs 4-bit).
- Auto-format on mount failure is optional and intentionally explicit.

## 4) Snapshot Save + Web Serving Flow

Current data flow after trigger/capture:
1. Camera capture happens in webcam path.
2. JPEG bytes are saved to SD snapshot directory.
3. HTTP APIs expose list and files.
4. Gallery page (`/snapshots`) renders thumbnails/images from those saved files.

Implemented endpoints:
- `GET /api/snapshots?limit=N` -> JSON list
- `GET /snapshots/<filename>` -> JPEG file
- `GET /snapshots` -> gallery page

Broken image issue that was fixed:
- Gallery had URL/query behavior that did not match file-serving route assumptions.
- Result: image tags rendered but browser got invalid responses.
- Fix: simplified URL handling and cache behavior so gallery requests map directly to valid file responses.

## 5) COM4 Monitoring and Debug Workflow

### Practical operating model

The monitor workflow was hardened so development is fast and repeatable:
- Before flash/monitor, close stale process that owns COM4.
- Use short monitor sessions for bring-up verification.
- Stop monitor automatically after startup stabilizes (3 seconds idle), unless crash loop is detected.

Crash-loop optimization:
- If logs clearly show panic/reset loop, do not wait for quiet timeout; stop immediately and iterate.

### Why this matters

Without this process:
- Flash can fail due to COM4 lock contention.
- Diagnosis slows down because logs are noisy and long-running.

With this process:
- Faster flash-debug cycles
- Deterministic startup validation
- Immediate attention to fatal loops

## 6) Wi-Fi Stability Decisions

Key fixes/choices for networking reliability:
- Removed Ethernet dependency paths for this project (Wi-Fi-only device intent)
- Kept reconnect handling practical (avoid tight loops)
- Preserved useful logs at boundaries (init, reconnect, fail states)
- Kept monitor-visible startup milestones for rapid triage

Critical lesson from earlier crash:
- Ethernet init path in example code caused `ESP_ERR_TIMEOUT` and panic (`ESP_ERROR_CHECK`) on this hardware intent.
- Removing non-required Ethernet path eliminated that class of failure.

## 7) UART/Flash Concurrency Guard

Enabled:
- `CONFIG_UART_ISR_IN_IRAM=y`

Reason:
- Reduces risk when UART logging/traffic coincides with flash/critical timing paths, especially under stress scenarios.

## 8) Current Known Behavior and Warnings

Observed warning:
- `spi_flash: Detected size(16384k) larger than the size in the binary image header(2048k)`

Meaning:
- Binary header flash size config mismatch; runtime uses header value.
- Not an immediate blocker, but should be aligned with intended flash configuration to avoid capacity/partition confusion.

Observed reset reason logging is enabled and useful:
- Startup logs include reset reason values to speed root-cause analysis after crashes/brownouts.

## 9) Recommended Day-to-Day Bring-Up Sequence

1. Confirm local config exists:
   - `main/include/catflapcam_config.h`
2. Build:
   - `idf.py build`
3. Flash:
   - `idf.py -p COM4 flash`
4. Monitor startup only:
   - run monitor, stop after 3s inactivity (or stop immediately on reset loop)
5. Validate runtime quickly:
   - web UI loads
   - stream works
   - capture triggers snapshot save
   - `/snapshots` gallery renders real images
6. If SD issues appear:
   - lower SD freq
   - verify slot/bus width config
   - verify card format and mount flags

## 10) Local Config Header (Kept Out of Git)

`main/include/catflapcam_config.h` is intentionally local-only.

Create from template:

```bash
cp main/include/catflapcam_config.example.h main/include/catflapcam_config.h
```

Set at minimum:

```c
#define CATFLAPCAM_WIFI_SSID "your-ssid"
#define CATFLAPCAM_WIFI_PASSWORD "your-password"
#define CATFLAPCAM_OTA_PASSWORD "your-ota-password"
```

And tune SD values for your board/card stability.

## 11) OTA Quick Reference

Endpoint:
- `POST /api/ota`

Auth header:
- `X-OTA-Password: <CATFLAPCAM_OTA_PASSWORD>`

Example:

```bash
curl -X POST \
  -H "X-OTA-Password: your-ota-password" \
  --data-binary "@build/catflapcam.bin" \
  http://<device-ip>/api/ota
```

Expected:
- wrong/missing password -> `401`
- valid upload -> `OK` then reboot

## 12) Bottom Line

The project is now operating with:
- Stable Wi-Fi bring-up (without unrelated Ethernet failure paths)
- Stable SD snapshot persistence and retrieval
- Practical, fast COM4 debug loops

The key to success was treating SD, Wi-Fi, and monitor access as one integrated system: slot/resource correctness, init order, and disciplined serial workflow.
