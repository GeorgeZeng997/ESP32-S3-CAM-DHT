#include <Arduino.h>
#include "esp_camera.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include <FS.h>
#include <SD_MMC.h>
#include "esp_camera.h"

#define CAMERA_MODEL_ESP32S3_EYE
#include "camera_pins.h"
#include "sd_utils.h"

// DHT11 data pin (set via build flag DHT11_PIN, defaults to GPIO 4).
constexpr uint8_t kDhtPin = static_cast<uint8_t>(DHT11_PIN);

// Capture + sensor cadence and storage guardrails.
constexpr uint32_t kCycleIntervalMs = 30000;  // 30 seconds between wakeups
constexpr uint64_t kMinimumFreeSpace = 2ULL * 1024ULL * 1024ULL;  // keep 2MB free on TF card

RTC_DATA_ATTR uint32_t gFrameIndex = 0;
RTC_DATA_ATTR uint32_t gReadingIndex = 0;
RTC_DATA_ATTR uint16_t gRunId = 0;

static String sessionDir = "/data";

// Simple rolling average smoothing for DHT11 readings.
struct SampleSmoother {
  static constexpr size_t kWindow = 4;
  int temps[kWindow] = {0};
  int hums[kWindow] = {0};
  size_t count = 0;
  size_t idx = 0;

  void add(int t, int h) {
    temps[idx] = t;
    hums[idx] = h;
    idx = (idx + 1) % kWindow;
    if (count < kWindow) {
      ++count;
    }
  }

  int avgTemp() const {
    if (count == 0) return 0;
    int sum = 0;
    for (size_t i = 0; i < count; ++i) sum += temps[i];
    return (sum + static_cast<int>(count / 2)) / static_cast<int>(count);
  }

  int avgHum() const {
    if (count == 0) return 0;
    int sum = 0;
    for (size_t i = 0; i < count; ++i) sum += hums[i];
    return (sum + static_cast<int>(count / 2)) / static_cast<int>(count);
  }
} gSmoother;

// Expect a pulse of a given level and return the number of loops observed.
// Returns 0 on timeout.
static uint32_t expectPulse(bool level) {
  constexpr uint32_t kMaxCycles = 12000;  // ~120us guard
  uint32_t count = 0;
  while (digitalRead(kDhtPin) == level) {
    if (++count >= kMaxCycles) {
      return 0;
    }
  }
  return count;
}

// Read temperature (C) and humidity (%) from DHT11. Returns true on success.
static bool readDht11(int &temperatureC, int &humidity) {
  uint8_t data[5] = {0, 0, 0, 0, 0};

  // Start signal.
  pinMode(kDhtPin, OUTPUT);
  digitalWrite(kDhtPin, HIGH);
  delay(250);
  digitalWrite(kDhtPin, LOW);
  delay(20);
  digitalWrite(kDhtPin, HIGH);
  delayMicroseconds(40);
  pinMode(kDhtPin, INPUT_PULLUP);

  if (!expectPulse(LOW) || !expectPulse(HIGH)) {
    return false;
  }

  for (int i = 0; i < 40; ++i) {
    uint32_t lowCycles = expectPulse(LOW);
    if (!lowCycles) {
      return false;
    }
    uint32_t highCycles = expectPulse(HIGH);
    if (!highCycles) {
      return false;
    }
    data[i / 8] <<= 1;
    if (highCycles > lowCycles) {
      data[i / 8] |= 1;
    }
  }

  uint8_t checksum = (data[0] + data[1] + data[2] + data[3]) & 0xFF;
  if (checksum != data[4]) {
    return false;
  }

  humidity = data[0];
  temperatureC = data[2];
  return true;
}

static bool readDht11WithRetry(int &temperatureC, int &humidity) {
  constexpr int kAttempts = 3;
  for (int i = 0; i < kAttempts; ++i) {
    if (readDht11(temperatureC, humidity)) {
      return true;
    }
    delay(50);
  }
  return false;
}

static bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  // OV5640 supports 5MP; target QSXGA (2592x1944) when PSRAM is present.
  config.frame_size = FRAMESIZE_SVGA;  // safe default until PSRAM confirmed
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 2;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_QSXGA;  // 5MP (2592x1944)
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
    Serial.println("PSRAM found and used");
  } else {
    // Without PSRAM, keep a modest frame size that fits DRAM.
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
    config.jpeg_quality = 14;
    Serial.println("PSRAM not found; using DRAM frame buffer");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, 0);
    s->set_gain_ctrl(s, 1);
    s->set_exposure_ctrl(s, 1);
    Serial.println("Camera sensor configured");
  }
  return true;
}

static bool appendReading(int tempC, int hum) {
  char path[64];
  snprintf(path, sizeof(path), "%s/readings.csv", sessionDir.c_str());
  File file = SD_MMC.open(path, FILE_APPEND);
  if (!file) {
    Serial.printf("Failed to open %s for append\n", path);
    return false;
  }
  uint64_t nowMs = esp_timer_get_time() / 1000ULL;
  file.printf("%lu,%lu,%llu,%d,%d\n",
              static_cast<unsigned long>(gRunId),
              static_cast<unsigned long>(gReadingIndex),
              static_cast<unsigned long long>(nowMs),
              tempC,
              hum);
  file.close();
  return true;
}

// Put OV5640 into software power-down to save a bit more when staying powered.
static void setCameraSoftPd(bool enable) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || s->id.PID != OV5640_PID) {
    return;
  }
  int reg = s->get_reg(s, 0x3008, 0xFF);
  if (reg < 0) {
    return;
  }
  if (enable) {
    reg |= 0x40;  // Bit6 = software power down.
  } else {
    reg &= ~0x40;
  }
  s->set_reg(s, 0x3008, 0xFF, reg);
}

static void sleepUntilNextCycle() {
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(kCycleIntervalMs) * 1000ULL);
  Serial.printf("Sleeping for %u ms\n\n", kCycleIntervalMs);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32-S3 CAM + DHT11 logger");

  if (!initSdCard()) {
    Serial.println("SD init failed; sleeping");
    sleepUntilNextCycle();
  }
  if (!ensureDir("/data")) {
    Serial.println("Failed to create /data; sleeping");
    sleepUntilNextCycle();
  }

  if (gRunId == 0) {
    gRunId = static_cast<uint16_t>(esp_random() & 0xFFFF);
  }
  char dirBuf[32];
  snprintf(dirBuf, sizeof(dirBuf), "/data/run_%04x", gRunId);
  if (!ensureDir(dirBuf)) {
    Serial.println("Failed to create run directory; sleeping");
    sleepUntilNextCycle();
  }
  sessionDir = dirBuf;

  if (!initCamera()) {
    Serial.println("Camera init failed; sleeping");
    sleepUntilNextCycle();
  }

  uint64_t freeBytes = sdFreeBytes();
  if (freeBytes < kMinimumFreeSpace) {
    Serial.println("Not enough free space on TF card; sleeping");
    sleepUntilNextCycle();
  }

  // Capture frame.
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed; sleeping");
    sleepUntilNextCycle();
  }

  freeBytes = sdFreeBytes();
  if (freeBytes < fb->len + kMinimumFreeSpace) {
    Serial.println("Not enough space for this frame; sleeping");
    esp_camera_fb_return(fb);
    sleepUntilNextCycle();
  }

  Serial.printf("Captured frame %ux%u (%u bytes)\n",
                fb->width, fb->height, static_cast<unsigned>(fb->len));
  String savedPath;
  if (saveJpegFrame(sessionDir.c_str(), gFrameIndex++, fb->buf, fb->len, savedPath)) {
    Serial.printf("Saved %s (%u bytes)\n", savedPath.c_str(), static_cast<unsigned>(fb->len));
  } else {
    Serial.println("Failed to write frame");
  }
  esp_camera_fb_return(fb);

  // Read temperature/humidity.
  pinMode(kDhtPin, INPUT_PULLUP);
  int temperatureC = 0;
  int humidity = 0;
  if (readDht11WithRetry(temperatureC, humidity)) {
    gSmoother.add(temperatureC, humidity);
    int smoothTemp = gSmoother.avgTemp();
    int smoothHum = gSmoother.avgHum();
    if (appendReading(smoothTemp, smoothHum)) {
      Serial.printf("Logged T=%dC H=%d%% (raw %d/%d)\n",
                    smoothTemp, smoothHum, temperatureC, humidity);
      ++gReadingIndex;
    } else {
      Serial.println("Failed to append reading");
    }
  } else {
    Serial.println("DHT11 read failed");
  }

  // Enter camera soft power-down before MCU deep sleep to trim standby draw.
  setCameraSoftPd(true);
  sleepUntilNextCycle();
}

void loop() {
  // Not used; all work is done on wake then deep sleep.
}
