#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include <FS.h>
#include <SD_MMC.h>

#ifndef CAMERA_MODEL_ESP32S3_EYE
#define CAMERA_MODEL_ESP32S3_EYE  // default; can be overridden via build_flags
#endif
#include "camera_pins.h"
#include "sd_utils.h"

// ----------------- Configuration constants -----------------
static const uint32_t kDefaultCycleIntervalMs = 30000;  // capture cadence default
static const uint64_t kDefaultMinimumFreeSpace = 2ULL * 1024ULL * 1024ULL;  // keep 2MB free
static const uint32_t kMinCycleMs = 5000;     // 5s lower bound
static const uint32_t kMaxCycleMs = 600000;   // 10min upper bound
static const uint32_t kMinFreeMb = 1;         // 1MB lower bound
static const uint32_t kMaxFreeMb = 512;       // 512MB upper bound
static const char *kConfigUser = "admin";       // Basic auth for config page
static const char *kConfigPass = "admin123";
static const char *kDefaultApSsid = "ESP32CAM-SETUP";
static const char *kDefaultApPass = "esp32setup";
static const char *kPrefsNs = "cfg";

// DHT11 data pin (set via build flag DHT11_PIN, defaults to GPIO 4).
constexpr uint8_t kDhtPin = static_cast<uint8_t>(DHT11_PIN);

// ----------------- State -----------------
static Preferences gPrefs;
static WebServer gServer(80);

static String gStaSsid;
static String gStaPass;
static String gApSsid = kDefaultApSsid;
static String gApPass = kDefaultApPass;
static String gToken = "changeme";
static bool gApMode = false;
static uint32_t gCycleIntervalMs = kDefaultCycleIntervalMs;
static uint64_t gMinimumFreeSpace = kDefaultMinimumFreeSpace;

static String sessionDir = "/data";
static String gLastFramePath;
static uint32_t gFrameIndex = 0;
static uint32_t gReadingIndex = 0;
static uint32_t gRunIndex = 0;
static bool gCameraReady = false;
static bool gSdReadBenchDone = false;

// ----------------- Utilities -----------------
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
    if (count < kWindow) ++count;
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

static uint32_t expectPulse(bool level) {
  constexpr uint32_t kMaxCycles = 12000;  // ~120us guard
  uint32_t count = 0;
  while (digitalRead(kDhtPin) == level) {
    if (++count >= kMaxCycles) return 0;
  }
  return count;
}

static bool readDht11(int &temperatureC, int &humidity) {
  uint8_t data[5] = {0, 0, 0, 0, 0};
  pinMode(kDhtPin, OUTPUT);
  digitalWrite(kDhtPin, HIGH);
  delay(250);
  digitalWrite(kDhtPin, LOW);
  delay(20);
  digitalWrite(kDhtPin, HIGH);
  delayMicroseconds(40);
  pinMode(kDhtPin, INPUT_PULLUP);

  if (!expectPulse(LOW) || !expectPulse(HIGH)) return false;

  for (int i = 0; i < 40; ++i) {
    uint32_t lowCycles = expectPulse(LOW);
    if (!lowCycles) return false;
    uint32_t highCycles = expectPulse(HIGH);
    if (!highCycles) return false;
    data[i / 8] <<= 1;
    if (highCycles > lowCycles) data[i / 8] |= 1;
  }

  uint8_t checksum = (data[0] + data[1] + data[2] + data[3]) & 0xFF;
  if (checksum != data[4]) return false;

  humidity = data[0];
  temperatureC = data[2];
  return true;
}

static bool readDht11WithRetry(int &temperatureC, int &humidity) {
  constexpr int kAttempts = 3;
  for (int i = 0; i < kAttempts; ++i) {
    if (readDht11(temperatureC, humidity)) return true;
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
  config.frame_size = FRAMESIZE_SVGA;
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

static bool ensureCameraReady() {
  if (gCameraReady) return true;
  Serial.println("Bringing camera up");
  if (initCamera()) {
    gCameraReady = true;
    Serial.println("Camera ready");
    return true;
  }
  Serial.println("Camera init failed");
  return false;
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
              static_cast<unsigned long>(gRunIndex),
              static_cast<unsigned long>(gReadingIndex),
              static_cast<unsigned long long>(nowMs),
              tempC,
              hum);
  file.close();
  return true;
}

static void setCameraSoftPd(bool enable) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || s->id.PID != OV5640_PID) return;
  int reg = s->get_reg(s, 0x3008, 0xFF);
  if (reg < 0) return;
  if (enable) {
    reg |= 0x40;  // Bit6 = software power down.
  } else {
    reg &= ~0x40;
  }
  s->set_reg(s, 0x3008, 0xFF, reg);
}

static void powerDownCamera() {
  setCameraSoftPd(true);
  esp_camera_deinit();
  gCameraReady = false;
  Serial.println("Camera powered down");
}

static void benchmarkSdRead(const char *path) {
  if (gSdReadBenchDone) return;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    Serial.printf("SD bench: failed to open %s\n", path);
    return;
  }
  constexpr size_t kBufSize = 4096;
  uint8_t buf[kBufSize];
  size_t total = 0;
  int64_t startUs = esp_timer_get_time();
  while (true) {
    int read = f.read(buf, kBufSize);
    if (read <= 0) break;
    total += static_cast<size_t>(read);
  }
  int64_t elapsedUs = esp_timer_get_time() - startUs;
  f.close();
  double elapsedMs = elapsedUs / 1000.0;
  double kbPerSec = (elapsedUs > 0) ? (total * 1000.0 / elapsedMs / 1024.0) : 0.0;
  double mbPerSec = kbPerSec / 1024.0;
  Serial.printf("SD bench: read %u bytes from %s in %.2f ms (%.2f KB/s, %.2f MB/s)\n",
                static_cast<unsigned>(total), path, elapsedMs, kbPerSec, mbPerSec);
  gSdReadBenchDone = true;
}

// ----------------- Wi-Fi + HTTP -----------------

static String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 4);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += c; break;
    }
  }
  return out;
}

static bool requireAuth() {
  // Auth disabled: allow all requests.
  return true;
}

static void handleListFrames() {
  if (!requireAuth()) return;
  const int page = gServer.hasArg("page") ? gServer.arg("page").toInt() : 1;
  const int pageSize = gServer.hasArg("page_size") ? gServer.arg("page_size").toInt() : 50;
  unsigned long t0 = millis();
  const int startIndex = (page - 1) * pageSize;
  int sent = 0;
  int skipped = 0;

  File root = SD_MMC.open("/data");
  if (!root) {
    gServer.send(500, "application/json", "{\"error\":\"no /data\"}");
    return;
  }

  String payload = "{\"items\":[";
  bool first = true;
  File runDir = root.openNextFile();
  while (runDir) {
    if (runDir.isDirectory()) {
      String runName = runDir.name();  // e.g. /data/run_0001
      File f = runDir.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          if (skipped < startIndex) {
            ++skipped;
          } else if (sent < pageSize) {
            if (!first) payload += ",";
            payload += "{\"run\":\"" + jsonEscape(runName.substring(runName.lastIndexOf('/') + 1)) + "\",";
            payload += "\"file\":\"" + jsonEscape(f.name()) + "\",";
            payload += "\"size\":" + String((unsigned long)f.size()) + "}";
            first = false;
            ++sent;
          }
        }
        f.close();
        if (sent >= pageSize) break;
        f = runDir.openNextFile();
      }
    }
    if (sent >= pageSize) break;
    runDir.close();
    runDir = root.openNextFile();
  }
  if (runDir) runDir.close();
  root.close();

  payload += "],\"has_more\":";
  payload += ((sent == pageSize) ? "true" : "false");
  payload += "}";
  gServer.send(200, "application/json", payload);
  Serial.printf("HTTP /frames page=%d size=%d -> items=%d (took %lums)\n",
                page, pageSize, sent, millis() - t0);
}

static void handleLatest() {
  if (!requireAuth()) return;
  unsigned long t0 = millis();
  if (gLastFramePath.isEmpty()) {
    gServer.send(404, "application/json", "{\"error\":\"no frames yet\"}");
    Serial.printf("HTTP /frames/latest -> 404 (no frame) in %lums\n", millis() - t0);
    return;
  }
  File f = SD_MMC.open(gLastFramePath.c_str(), FILE_READ);
  if (!f) {
    gServer.send(404, "application/json", "{\"error\":\"missing file\"}");
    Serial.printf("HTTP /frames/latest -> 404 (missing file) in %lums\n", millis() - t0);
    return;
  }
  Serial.printf("HTTP /frames/latest streaming %s (%u bytes)\n",
                gLastFramePath.c_str(), static_cast<unsigned>(f.size()));
  gServer.streamFile(f, "image/jpeg");
  f.close();
  Serial.printf("HTTP /frames/latest done in %lums\n", millis() - t0);
}

static void handleFetchFrame() {
  if (!requireAuth()) return;
  unsigned long t0 = millis();
  if (!gServer.hasArg("run") || !gServer.hasArg("file")) {
    gServer.send(400, "application/json", "{\"error\":\"missing run or file\"}");
    Serial.printf("HTTP /frames/file -> 400 (missing args) in %lums\n", millis() - t0);
    return;
  }
  String path = "/data/";
  path += gServer.arg("run");
  path += "/";
  path += gServer.arg("file");
  File f = SD_MMC.open(path.c_str(), FILE_READ);
  if (!f) {
    gServer.send(404, "application/json", "{\"error\":\"not found\"}");
    Serial.printf("HTTP /frames/file %s -> 404 in %lums\n", path.c_str(), millis() - t0);
    return;
  }
  Serial.printf("HTTP /frames/file %s (%u bytes)\n",
                path.c_str(), static_cast<unsigned>(f.size()));
  gServer.streamFile(f, "image/jpeg");
  f.close();
  Serial.printf("HTTP /frames/file done in %lums\n", millis() - t0);
}

static bool checkConfigAuth() {
  // Auth disabled: allow config page without Basic auth.
  return true;
}

static void handleConfigForm() {
  if (!checkConfigAuth()) return;
  String html = "<html><body><h3>ESP32-S3-CAM-DHT Setup</h3>"
                "<form method='POST' action='/config'>"
                "Mode: <select name='mode'>"
                "<option value='sta'" + String(gApMode ? "" : " selected") + ">STA</option>"
                "<option value='ap'" + String(gApMode ? " selected" : "") + ">AP</option>"
                "</select><br/>"
                "STA SSID: <input name='ssid' value='" + gStaSsid + "'/><br/>"
                "STA Password: <input type='password' name='pass' value='" + gStaPass + "'/><br/>"
                "AP SSID: <input name='ap_ssid' value='" + gApSsid + "'/><br/>"
                "AP Password: <input type='password' name='ap_pass' value='" + gApPass + "'/><br/>"
                "Cycle (ms): <input name='cycle_ms' value='" + String(gCycleIntervalMs) + "'/><br/>"
                "Min free (MB): <input name='min_free_mb' value='" + String((unsigned long)(gMinimumFreeSpace / (1024 * 1024))) + "'/><br/>"
                "Token: <input type='password' name='token' value='" + gToken + "'/><br/>"
                "<input type='submit' value='Save'/>"
                "</form></body></html>";
  gServer.send(200, "text/html", html);
}

static uint32_t sanitizeCycleMs(uint32_t v) {
  if (v < kMinCycleMs || v > kMaxCycleMs) return kDefaultCycleIntervalMs;
  return v;
}

static uint64_t sanitizeMinFreeBytes(uint32_t mb) {
  if (mb < kMinFreeMb || mb > kMaxFreeMb) return kDefaultMinimumFreeSpace;
  return static_cast<uint64_t>(mb) * 1024ULL * 1024ULL;
}

static void handleConfigPost() {
  if (!checkConfigAuth()) return;
  String mode = gServer.arg("mode");
  gApMode = (mode == "ap");
  gStaSsid = gServer.arg("ssid");
  gStaPass = gServer.arg("pass");
  gApSsid = gServer.arg("ap_ssid");
  gApPass = gServer.arg("ap_pass");
  gToken = gServer.arg("token");

  uint32_t newCycle = sanitizeCycleMs(gServer.arg("cycle_ms").toInt());
  uint64_t newMinFree = sanitizeMinFreeBytes(gServer.arg("min_free_mb").toInt());
  gCycleIntervalMs = newCycle;
  gMinimumFreeSpace = newMinFree;

  gPrefs.begin(kPrefsNs, false);
  gPrefs.putString("mode", gApMode ? "ap" : "sta");
  gPrefs.putString("ssid", gStaSsid);
  gPrefs.putString("pass", gStaPass);
  gPrefs.putString("ap_ssid", gApSsid);
  gPrefs.putString("ap_pass", gApPass);
  gPrefs.putString("token", gToken);
  gPrefs.putULong("cycle_ms", gCycleIntervalMs);
  gPrefs.putULong("min_free_mb", static_cast<uint32_t>(gMinimumFreeSpace / (1024 * 1024)));
  gPrefs.end();

  gServer.send(200, "text/plain", "Saved. Reboot device.");
}

static void handleBrowse() {
  // Simple HTML browser for manual download without token.
  String html = "<html><body><h3>Files</h3><ul>";
  File root = SD_MMC.open("/data");
  if (!root) {
    gServer.send(500, "text/plain", "SD not ready");
    return;
  }
  File runDir = root.openNextFile();
  while (runDir) {
    if (runDir.isDirectory()) {
      String runName = runDir.name();
      html += "<li>" + runName + "<ul>";
      File f = runDir.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          String fileName = f.name();
          html += "<li><a href=\"/frames/file?run=";
          html += runName.substring(runName.lastIndexOf('/') + 1);
          html += "&file=";
          html += fileName;
          html += "\">";
          html += fileName;
          html += "</a> (" + String((unsigned long)f.size()) + " bytes)</li>";
        }
        f.close();
        f = runDir.openNextFile();
      }
      html += "</ul></li>";
    }
    runDir.close();
    runDir = root.openNextFile();
  }
  root.close();
  html += "</ul></body></html>";
  gServer.send(200, "text/html", html);
}

static void registerHttpHandlers() {
  gServer.on("/frames", HTTP_GET, handleListFrames);
  gServer.on("/frames/latest", HTTP_GET, handleLatest);
  gServer.on("/frames/file", HTTP_GET, handleFetchFrame);
  gServer.on("/config", HTTP_GET, handleConfigForm);
  gServer.on("/config", HTTP_POST, handleConfigPost);
  gServer.on("/browse", HTTP_GET, handleBrowse);
  gServer.begin();
  Serial.println("HTTP server started");
}

static void loadPrefs() {
  gPrefs.begin(kPrefsNs, true);
  gApMode = gPrefs.getString("mode", "sta") == "ap";
  gStaSsid = gPrefs.getString("ssid", "");
  gStaPass = gPrefs.getString("pass", "");
  gApSsid = gPrefs.getString("ap_ssid", kDefaultApSsid);
  gApPass = gPrefs.getString("ap_pass", kDefaultApPass);
  gToken = gPrefs.getString("token", "changeme");
  uint32_t storedCycle = gPrefs.getULong("cycle_ms", kDefaultCycleIntervalMs);
  uint32_t storedMinFreeMb = gPrefs.getULong("min_free_mb", static_cast<uint32_t>(kDefaultMinimumFreeSpace / (1024 * 1024)));
  gPrefs.end();
  gCycleIntervalMs = sanitizeCycleMs(storedCycle);
  gMinimumFreeSpace = sanitizeMinFreeBytes(storedMinFreeMb);
}

static void startApConfigPortal() {
  gApMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(gApSsid.c_str(), gApPass.c_str());
  Serial.printf("AP mode. SSID: %s, IP: %s\n", gApSsid.c_str(), WiFi.softAPIP().toString().c_str());
  registerHttpHandlers();
}

static bool connectStaWithTimeout(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(gStaSsid.c_str(), gStaPass.c_str());
  Serial.printf("Connecting to %s\n", gStaSsid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("STA connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("STA connect timeout");
  return false;
}

// ----------------- Setup & loop -----------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32-S3 CAM + DHT11 logger with HTTP file access");
  loadPrefs();

  if (!initSdCard()) {
    Serial.println("SD init failed; halt");
    return;
  }
  if (!ensureDir("/data")) {
    Serial.println("Failed to create /data; halt");
    return;
  }

  // Determine next run directory by scanning existing run_* folders.
  File root = SD_MMC.open("/data");
  uint32_t maxRun = 0;
  if (root) {
    File d = root.openNextFile();
    while (d) {
      if (d.isDirectory()) {
        String name = d.name();  // e.g. /data/run_0005
        int idx = name.lastIndexOf('_');
        if (idx >= 0) {
          String numStr = name.substring(idx + 1);
          uint32_t val = static_cast<uint32_t>(strtoul(numStr.c_str(), nullptr, 10));
          if (val > maxRun) maxRun = val;
        }
      }
      d.close();
      d = root.openNextFile();
    }
    root.close();
  }
  gRunIndex = maxRun + 1;
  char dirBuf[32];
  snprintf(dirBuf, sizeof(dirBuf), "/data/run_%04lu", static_cast<unsigned long>(gRunIndex));
  if (!ensureDir(dirBuf)) {
    Serial.println("Failed to create run directory; halt");
    return;
  }
  sessionDir = dirBuf;
  Serial.printf("Session dir: %s\n", sessionDir.c_str());

  if (!ensureCameraReady()) {
    Serial.println("Camera init failed; halt");
    return;
  }

  if (!gApMode && !gStaSsid.isEmpty() && connectStaWithTimeout(15000)) {
    Serial.println("Using STA mode");
    registerHttpHandlers();
  } else {
    Serial.println("Falling back to AP config");
    startApConfigPortal();
  }

  // First capture immediately.
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) {
    uint64_t freeBytes = sdFreeBytes();
    if (freeBytes >= fb->len + gMinimumFreeSpace) {
      String savedPath;
      if (saveJpegFrame(sessionDir.c_str(), gFrameIndex++, fb->buf, fb->len, savedPath)) {
        gLastFramePath = savedPath;
        Serial.printf("Saved %s (%u bytes)\n", savedPath.c_str(), static_cast<unsigned>(fb->len));
        benchmarkSdRead(gLastFramePath.c_str());
      } else {
        Serial.println("Failed to write frame");
      }
    } else {
      Serial.println("Not enough space for initial frame");
    }
    esp_camera_fb_return(fb);
  }
  powerDownCamera();
}

void loop() {
  static uint32_t lastCycleMs = 0;
  const uint32_t now = millis();

  if (now - lastCycleMs >= gCycleIntervalMs) {
    lastCycleMs = now;

    uint64_t freeBytes = sdFreeBytes();
    if (freeBytes < gMinimumFreeSpace) {
      Serial.println("Not enough free space on TF card; skipping capture");
    } else {
      if (!ensureCameraReady()) {
        Serial.println("Camera init failed; skipping capture");
        goto readings;
      }
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        freeBytes = sdFreeBytes();
        if (freeBytes >= fb->len + gMinimumFreeSpace) {
          String savedPath;
          if (saveJpegFrame(sessionDir.c_str(), gFrameIndex++, fb->buf, fb->len, savedPath)) {
            gLastFramePath = savedPath;
            Serial.printf("Saved %s (%u bytes)\n", savedPath.c_str(), static_cast<unsigned>(fb->len));
          } else {
            Serial.println("Failed to write frame");
          }
        } else {
          Serial.println("Not enough space for this frame");
        }
        esp_camera_fb_return(fb);
      } else {
        Serial.println("Camera capture failed");
      }
    }
    powerDownCamera();

readings:
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

  }

  gServer.handleClient();
}
