# ESP32-S3 CAM Logger Android Client

Jetpack Compose Android application for browsing time-lapse frames recorded by `ESP32-S3-CAM-DHT` over HTTP, previewing captured JPEGs, and encoding any `run_XXXX` capture session into an H.264 `.mp4` video on-device using `MediaCodec` + `MediaMuxer`.

## Features

- **Authenticated Frame Browser**: Connects to the ESP32-S3 HTTP server (`http://192.168.4.1` in SoftAP mode or its LAN IP in STA mode) with optional `X-Auth-Token` header authentication.
- **Live JPEG Preview**: Streams and decodes individual frames from `/frames/file`.
- **On-Device H.264 MP4 Time-Lapse Export**: Downloads all `.jpg` frames for a selected run, normalizes dimensions to H.264-compliant even resolution, converts frames to `NV21` (`YUV420`), and muxes an `.mp4` clip into the app cache directory.

## Build & Run

1. Open `android-app/` in **Android Studio** (AGP 8.5+, JDK 17) or build from the command line:
   ```bash
   cd android-app
   ./gradlew assembleDebug
   ```
2. Connect your phone to the ESP32-S3 SoftAP (`ESP32S3-CAM-SETUP`) or the same Wi-Fi LAN.
3. Enter the **Device Base URL** and optional **X-Auth-Token**, tap **Save Settings**, and tap **Refresh Frames**.
4. Tap **Preview** on any frame or **Export Run MP4** to render a time-lapse video.
