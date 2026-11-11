# ESP32-S3-CAM + DHT11 TF Card Logger & HTTP Server

Interval time-lapse JPEG & DHT11 temperature/humidity logger for **ESP32-S3-CAM**, featuring 1-bit `SD_MMC` storage, automatic camera software power-down between captures, an authenticated HTTP file/configuration server, and a companion **Android Jetpack Compose** viewer + H.264 MP4 time-lapse exporter.

## Hardware

- **Board**: `ESP32-S3-CAM` (`CAMERA_MODEL_ESP32S3_EYE` by default; see `src/camera_pins.h`).
- **Storage**: MicroSD / TF card over `SD_MMC` 1-bit high-speed interface (`CLK=39`, `CMD=38`, `D0=40`).
- **Sensor**: `DHT11` single-wire temperature/humidity sensor on `GPIO 21` (`-DDHT11_PIN=21` in `platformio.ini`, `3.3 V` supply).

## Build & Flash

1. *(Optional)* Copy `include/wifi_config.example.h` to `include/wifi_config.h` (git-ignored) to pre-populate initial STA/AP/Auth defaults, or configure everything at runtime via `http://192.168.4.1/config`.
2. Build and upload with PlatformIO:
   ```bash
   pio run
   pio run -t upload
   pio device monitor -b 115200
   ```

## Runtime Architecture

- **Monotonic Session Directories**: Scans `/data/run_*` on boot and creates the next `/data/run_XXXX` directory, writing `frame_000000.jpg`, `frame_000001.jpg`, ... alongside `readings.csv` (`runId,readingIdx,ms,tempC,hum`).
- **Low-Power Camera Cycling**: Initializes OV5640 (`QSXGA 2592x1944` when PSRAM is detected, `SVGA` fallback otherwise), captures a JPEG frame, and places the camera into software power-down (`esp_camera_deinit()`) until the next interval (`30 s` default).
- **Space Guard**: Skips JPEG writes whenever free SD space drops below the configured reserve (`2 MB` default).
- **Non-Blocking Wi-Fi Management**: Connects to STA if configured (with non-blocking 15-second background reconnects if the router drops), or falls back to SoftAP (`ESP32S3-CAM-SETUP` at `192.168.4.1`).

## HTTP Endpoints

| Endpoint | Method | Auth | Description |
| --- | --- | --- | --- |
| `/api/status` | `GET` | `X-Auth-Token` (if set) | Live JSON status (`run`, `frames`, `readings`, `temp_c`, `hum_pct`, `sd_free_mb`, `wifi_mode`, `ip`, `uptime_ms`) |
| `/frames?page=1&page_size=20` | `GET` | `X-Auth-Token` (if set) | Paginated JSON list of recorded frames across runs |
| `/frames/latest` | `GET` | `X-Auth-Token` (if set) | Stream the most recently captured JPEG |
| `/frames/file?run=run_0001&file=frame_000000.jpg` | `GET` | `X-Auth-Token` (if set) | Stream a validated file from `/data/<run>/<file>` |
| `/browse` | `GET` | `X-Auth-Token` (if set) | Minimal HTML directory browser |
| `/config` | `GET` / `POST` | HTTP Basic Auth (if `cfg_pass` set) | Update STA/AP credentials, capture interval, free-space floor, API token, and admin password in NVS |

## Companion Android App

See [`android-app/README.md`](android-app/README.md) to browse frames over Wi-Fi, preview JPEGs, and export any `run_XXXX` sequence directly to an H.264 `.mp4` time-lapse video on Android 8.0+.
