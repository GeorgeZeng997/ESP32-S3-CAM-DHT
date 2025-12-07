# ESP32-S3-CAM + DHT11 TF Logger

Capture a JPEG and read DHT11 temperature/humidity on each wake, store everything to the TF card, then immediately deep sleep for low-power logging.

## Hardware
- Board: ESP32-S3-CAM (example uses ESP32-S3 EYE pinout; see `src/camera_pins.h`).
- TF card: SD_MMC interface (CLK=39, CMD=38, D0=40).
- Sensor: DHT11 data pin defaults to GPIO 21 (set via `-DDHT11_PIN` in `platformio.ini`; 3.3V supply).

## Build & Flash
```bash
cd ESP32-S3-CAM-DHT
pio run                      # build
pio run -t upload            # flash
pio device monitor -b 115200 # serial monitor
```

## Runtime Behavior
- On first boot creates `/data/run_xxxx/`, saves `frame_000000.jpg` onward, and appends readings to `readings.csv` in format: `runId,readingIdx,ms,tempC,hum`.
- Cycle: wake -> init SD + camera -> capture JPEG -> read DHT11 -> write files -> `esp_deep_sleep_start()`; wakes again after the interval (default 30s).
- Space guard: if remaining space is below 2MB or insufficient for the next frame, skip capture and go back to sleep.

## Tuning
- Capture/reading interval: `kCycleIntervalMs` in `src/main.cpp`.
- Reserved free space: `kMinimumFreeSpace` in `src/main.cpp`.
- Camera quality/size: `initCamera()` targets OV5640. With PSRAM it uses QSXGA (2592x1944) quality 10; without PSRAM it falls back to SVGA, quality 14.
- Different S3-CAM pinouts: select `CAMERA_MODEL_*` in `platformio.ini` and update `src/camera_pins.h` accordingly.
