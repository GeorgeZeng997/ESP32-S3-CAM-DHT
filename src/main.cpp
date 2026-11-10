#include <Arduino.h>
#include <FS.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <WebServer.h>
#include <WiFi.h>
#include "esp_camera.h"

#ifndef CAMERA_MODEL_ESP32S3_EYE
#define CAMERA_MODEL_ESP32S3_EYE  // default; can be overridden via build_flags
#endif
#include "camera_pins.h"
#include "http_security.h"
#include "sd_utils.h"

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#include "wifi_config.example.h"
#endif

#ifndef DHT11_PIN
#define DHT11_PIN 21
#endif

// ----------------- Configuration constants -----------------
static const uint32_t kDefaultCycleIntervalMs = 30000;                        // 30s capture cadence default
static const uint64_t kDefaultMinimumFreeSpace = 2ULL * 1024ULL * 1024ULL;    // keep 2MB free
static const uint32_t kMinCycleMs = 5000;                                     // 5s lower bound
static const uint32_t kMaxCycleMs = 600000;                                   // 10min upper bound
static const uint32_t kMinFreeMb = 1;                                         // 1MB lower bound
static const uint32_t kMaxFreeMb = 512;                                       // 512MB upper bound
static const uint32_t kStaReconnectIntervalMs = 15000;                        // non-blocking STA retry interval
static const char *kPrefsNs = "cfg";

constexpr uint8_t kDhtPin = static_cast<uint8_t>(DHT11_PIN);

// ----------------- State -----------------
static Preferences gPrefs;
static WebServer gServer(80);
static portMUX_TYPE gDhtMux = portMUX_INITIALIZER_UNLOCKED;

static String gStaSsid = WIFI_CFG_DEFAULT_STA_SSID;
static String gStaPass = WIFI_CFG_DEFAULT_STA_PASS;
static String gApSsid = WIFI_CFG_DEFAULT_AP_SSID;
static String gApPass = WIFI_CFG_DEFAULT_AP_PASS;
static String gConfigUser = WIFI_CFG_DEFAULT_CONFIG_USER;
static String gConfigPass = WIFI_CFG_DEFAULT_CONFIG_PASS;
static String gToken = WIFI_CFG_DEFAULT_API_TOKEN;
static bool gApMode = false;
static uint32_t gCycleIntervalMs = kDefaultCycleIntervalMs;
static uint64_t gMinimumFreeSpace = kDefaultMinimumFreeSpace;

static String sessionDir = "/data";
static String gLastFramePath;
static uint32_t gFrameIndex = 0;
static uint32_t gReadingIndex = 0;
static uint32_t gRunIndex = 0;
static bool gCameraReady = false;
static bool gSdReady = false;
static bool gSdReadBenchDone = false;
static uint32_t gLastStaReconnectMs = 0;

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
  delay(20);
  digitalWrite(kDhtPin, LOW);
  delay(20);
  digitalWrite(kDhtPin, HIGH);
  delayMicroseconds(35);
  pinMode(kDhtPin, INPUT_PULLUP);

  uint32_t lowCycles[40];
  uint32_t highCycles[40];
  bool pulseOk = true;

  portENTER_CRITICAL(&gDhtMux);
  if (!expectPulse(LOW) || !expectPulse(HIGH)) {
    pulseOk = false;
  } else {
    for (int i = 0; i < 40; ++i) {
      lowCycles[i] = expectPulse(LOW);
      highCycles[i] = expectPulse(HIGH);
      if (lowCycles[i] == 0 || highCycles[i] == 0) {
        pulseOk = false;
        break;
      }
    }
  }
  portEXIT_CRITICAL(&gDhtMux);

  if (!pulseOk) return false;

  for (int i = 0; i < 40; ++i) {
    data[i / 8] <<= 1;
    if (highCycles[i] > lowCycles[i]) data[i / 8] |= 1U;
  }

  const uint8_t checksum = static_cast<uint8_t>((data[0] + data[1] + data[2] + data[3]) & 0xFFU);
  if (checksum != data[4]) return false;

  const int hum = static_cast<int>(data[0]);
  const int temp = ((data[3] & 0x80U) != 0U) ? -static_cast<int>(data[2]) : static_cast<int>(data[2]);
  if (hum < 1 || hum > 100 || temp < -20 || temp > 60) return false;

  humidity = hum;
  temperatureC = temp;
  return true;
}

static bool readDht11WithRetry(int &temperatureC, int &humidity) {
  constexpr int kAttempts = 3;
  for (int i = 0; i < kAttempts; ++i) {
    if (readDht11(temperatureC, humidity)) return true;
    delay(60);
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
  if (initCamera()) {
    gCameraReady = true;
    return true;
  }
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
    reg |= 0x40;
  } else {
    reg &= ~0x40;
  }
  s->set_reg(s, 0x3008, 0xFF, reg);
}

static void powerDownCamera() {
  if (!gCameraReady) return;
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

static void captureFrameOnce(bool runBenchmark) {
  uint64_t freeBytes = sdFreeBytes();
  if (freeBytes < gMinimumFreeSpace) {
    Serial.println("Not enough free space on TF card; skipping capture");
    return;
  }
  if (!ensureCameraReady()) {
    Serial.println("Camera init failed; skipping capture");
    return;
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) {
    freeBytes = sdFreeBytes();
    if (freeBytes >= fb->len + gMinimumFreeSpace) {
      String savedPath;
      if (saveJpegFrame(sessionDir.c_str(), gFrameIndex++, fb->buf, fb->len, savedPath)) {
        gLastFramePath = savedPath;
        Serial.printf("Saved %s (%u bytes)\n", savedPath.c_str(), static_cast<unsigned>(fb->len));
        if (runBenchmark) {
          benchmarkSdRead(gLastFramePath.c_str());
        }
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
  powerDownCamera();
}

static void recordDhtReadingOnce() {
  int temperatureC = 0;
  int humidity = 0;
  if (readDht11WithRetry(temperatureC, humidity)) {
    gSmoother.add(temperatureC, humidity);
    const int smoothTemp = gSmoother.avgTemp();
    const int smoothHum = gSmoother.avgHum();
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
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

static String htmlEscapeString(const String &in) {
  char buf[192];
  camsec::htmlEscapeToBuf(in.c_str(), buf, sizeof(buf));
  return String(buf);
}

static bool requireAuth() {
  // Enforce API token verification whenever gToken is configured (via X-Auth-Token header or ?token=).
  if (gToken.isEmpty()) {
    return true;
  }
  String candidate = gServer.header("X-Auth-Token");
  if (candidate.isEmpty() && gServer.hasArg("token")) {
    candidate = gServer.arg("token");
  }
  if (camsec::constantTimeEquals(candidate.c_str(), gToken.c_str())) {
    return true;
  }
  gServer.send(401, "application/json", "{\"error\":\"unauthorized\"}");
  return false;
}

static bool checkConfigAuth() {
  // Enforce HTTP Basic Auth on /config whenever a config password is set.
  if (gConfigPass.isEmpty()) {
    return true;
  }
  if (gServer.authenticate(gConfigUser.c_str(), gConfigPass.c_str())) {
    return true;
  }
  gServer.requestAuthentication();
  return false;
}

static void handleApiStatus() {
  if (!requireAuth()) return;
  const IPAddress ip = gApMode ? WiFi.softAPIP() : WiFi.localIP();
  const uint64_t freeMb = gSdReady ? (sdFreeBytes() / (1024ULL * 1024ULL)) : 0ULL;
  char payload[384];
  snprintf(payload, sizeof(payload),
           "{\"run\":%lu,\"frames\":%lu,\"readings\":%lu,"
           "\"temp_c\":%d,\"hum_pct\":%d,\"sd_free_mb\":%llu,"
           "\"wifi_mode\":\"%s\",\"ip\":\"%s\",\"sta_connected\":%s,"
           "\"cycle_ms\":%lu,\"free_heap\":%lu,\"uptime_ms\":%lu}",
           static_cast<unsigned long>(gRunIndex),
           static_cast<unsigned long>(gFrameIndex),
           static_cast<unsigned long>(gReadingIndex),
           gSmoother.avgTemp(),
           gSmoother.avgHum(),
           static_cast<unsigned long long>(freeMb),
           gApMode ? "ap" : "sta",
           ip.toString().c_str(),
           (WiFi.status() == WL_CONNECTED) ? "true" : "false",
           static_cast<unsigned long>(gCycleIntervalMs),
           static_cast<unsigned long>(ESP.getFreeHeap()),
           static_cast<unsigned long>(millis()));
  gServer.send(200, "application/json", payload);
}

static void handleListFrames() {
  if (!requireAuth()) return;
  const int page = camsec::clampPage(gServer.hasArg("page") ? gServer.arg("page").toInt() : 1);
  const int pageSize = camsec::clampPageSize(gServer.hasArg("page_size") ? gServer.arg("page_size").toInt() : 50);
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
      String runName = runDir.name();
      String runLeaf = runName.substring(runName.lastIndexOf('/') + 1);
      File f = runDir.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          String fName = f.name();
          String fileLeaf = fName.substring(fName.lastIndexOf('/') + 1);
          if (skipped < startIndex) {
            ++skipped;
          } else if (sent < pageSize) {
            if (!first) payload += ",";
            payload += "{\"run\":\"" + jsonEscape(runLeaf) + "\",";
            payload += "\"file\":\"" + jsonEscape(fileLeaf) + "\",";
            payload += "\"size\":" + String(static_cast<unsigned long>(f.size())) + "}";
            first = false;
            ++sent;
          }
        }
        f.close();
        if (sent >= pageSize) break;
        f = runDir.openNextFile();
      }
    }
    runDir.close();
    if (sent >= pageSize) break;
    runDir = root.openNextFile();
  }
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
    return;
  }
  File f = SD_MMC.open(gLastFramePath.c_str(), FILE_READ);
  if (!f) {
    gServer.send(404, "application/json", "{\"error\":\"missing file\"}");
    return;
  }
  gServer.streamFile(f, "image/jpeg");
  f.close();
  Serial.printf("HTTP /frames/latest done in %lums\n", millis() - t0);
}

static void handleFetchFrame() {
  if (!requireAuth()) return;
  unsigned long t0 = millis();
  if (!gServer.hasArg("run") || !gServer.hasArg("file")) {
    gServer.send(400, "application/json", "{\"error\":\"missing run or file\"}");
    return;
  }
  const String runArg = gServer.arg("run");
  const String fileArg = gServer.arg("file");
  if (!camsec::isValidSafeSegment(runArg.c_str()) || !camsec::isValidSafeSegment(fileArg.c_str())) {
    gServer.send(400, "application/json", "{\"error\":\"invalid run or file name\"}");
    return;
  }

  String path = "/data/" + runArg + "/" + fileArg;
  File f = SD_MMC.open(path.c_str(), FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    gServer.send(404, "application/json", "{\"error\":\"not found\"}");
    return;
  }
  gServer.streamFile(f, camsec::contentTypeForFilename(fileArg.c_str()));
  f.close();
  Serial.printf("HTTP /frames/file %s done in %lums\n", path.c_str(), millis() - t0);
}

static void handleConfigForm() {
  if (!checkConfigAuth()) return;
  // Never echo saved passwords or API tokens back into HTML source.
  String html = "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                "<title>ESP32-S3-CAM-DHT Setup</title></head><body>"
                "<h3>ESP32-S3-CAM-DHT Configuration</h3>"
                "<form method=\"POST\" action=\"/config\" autocomplete=\"off\">"
                "Mode: <select name=\"mode\">"
                "<option value=\"sta\"" + String(gApMode ? "" : " selected") + ">STA</option>"
                "<option value=\"ap\"" + String(gApMode ? " selected" : "") + ">AP</option>"
                "</select><br/>"
                "STA SSID: <input name=\"ssid\" maxlength=\"32\" value=\"" + htmlEscapeString(gStaSsid) + "\"/><br/>"
                "STA Password: <input type=\"password\" name=\"pass\" maxlength=\"64\" value=\"\" placeholder=\"(unchanged if blank)\"/><br/>"
                "AP SSID: <input name=\"ap_ssid\" maxlength=\"32\" value=\"" + htmlEscapeString(gApSsid) + "\"/><br/>"
                "AP Password: <input type=\"password\" name=\"ap_pass\" maxlength=\"64\" value=\"\" placeholder=\"(unchanged if blank; >=8 chars for WPA2)\"/><br/>"
                "Cycle (ms): <input name=\"cycle_ms\" value=\"" + String(gCycleIntervalMs) + "\"/><br/>"
                "Min free (MB): <input name=\"min_free_mb\" value=\"" + String(static_cast<unsigned long>(gMinimumFreeSpace / (1024ULL * 1024ULL))) + "\"/><br/>"
                "API Token: <input type=\"password\" name=\"token\" maxlength=\"64\" value=\"\" placeholder=\"(unchanged if blank)\"/><br/>"
                "Admin Password (/config): <input type=\"password\" name=\"cfg_pass\" maxlength=\"64\" value=\"\" placeholder=\"(unchanged if blank)\"/><br/>"
                "<input type=\"submit\" value=\"Save\"/>"
                "</form></body></html>";
  gServer.send(200, "text/html; charset=utf-8", html);
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
  gApMode = (gServer.arg("mode") == "ap");
  if (gServer.hasArg("ssid")) {
    gStaSsid = gServer.arg("ssid");
  }
  if (gServer.hasArg("ap_ssid") && !gServer.arg("ap_ssid").isEmpty()) {
    gApSsid = gServer.arg("ap_ssid");
  }
  // Preserve existing secrets when password fields are left blank on the form.
  if (gServer.hasArg("pass") && !gServer.arg("pass").isEmpty()) {
    gStaPass = gServer.arg("pass");
  }
  if (gServer.hasArg("ap_pass") && !gServer.arg("ap_pass").isEmpty()) {
    gApPass = gServer.arg("ap_pass");
  }
  if (gServer.hasArg("token") && !gServer.arg("token").isEmpty()) {
    gToken = gServer.arg("token");
  }
  if (gServer.hasArg("cfg_pass") && !gServer.arg("cfg_pass").isEmpty()) {
    gConfigPass = gServer.arg("cfg_pass");
  }

  gCycleIntervalMs = sanitizeCycleMs(static_cast<uint32_t>(gServer.arg("cycle_ms").toInt()));
  gMinimumFreeSpace = sanitizeMinFreeBytes(static_cast<uint32_t>(gServer.arg("min_free_mb").toInt()));

  gPrefs.begin(kPrefsNs, false);
  gPrefs.putString("mode", gApMode ? "ap" : "sta");
  gPrefs.putString("ssid", gStaSsid);
  gPrefs.putString("pass", gStaPass);
  gPrefs.putString("ap_ssid", gApSsid);
  gPrefs.putString("ap_pass", gApPass);
  gPrefs.putString("token", gToken);
  gPrefs.putString("cfg_pass", gConfigPass);
  gPrefs.putULong("cycle_ms", gCycleIntervalMs);
  gPrefs.putULong("min_free_mb", static_cast<uint32_t>(gMinimumFreeSpace / (1024ULL * 1024ULL)));
  gPrefs.end();

  gServer.send(200, "text/plain; charset=utf-8", "Configuration saved. Reboot the device to apply Wi-Fi mode changes.");
}

static void handleBrowse() {
  if (!requireAuth()) return;
  File root = SD_MMC.open("/data");
  if (!root) {
    gServer.send(500, "text/plain; charset=utf-8", "SD card not ready");
    return;
  }
  String html = "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"><title>Recorded Runs</title></head><body><h3>Recorded Files</h3><ul>";
  File runDir = root.openNextFile();
  while (runDir) {
    if (runDir.isDirectory()) {
      String runName = runDir.name();
      String runLeaf = runName.substring(runName.lastIndexOf('/') + 1);
      html += "<li>" + htmlEscapeString(runLeaf) + "<ul>";
      File f = runDir.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          String fName = f.name();
          String fileLeaf = fName.substring(fName.lastIndexOf('/') + 1);
          html += "<li><a href=\"/frames/file?run=" + htmlEscapeString(runLeaf) +
                  "&file=" + htmlEscapeString(fileLeaf) + "\">" +
                  htmlEscapeString(fileLeaf) + "</a> (" +
                  String(static_cast<unsigned long>(f.size())) + " bytes)</li>";
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
  gServer.send(200, "text/html; charset=utf-8", html);
}

static void registerHttpHandlers() {
  static const char *kCollectedHeaders[] = {"X-Auth-Token"};
  gServer.collectHeaders(kCollectedHeaders, 1);

  gServer.on("/api/status", HTTP_GET, handleApiStatus);
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
  gStaSsid = gPrefs.getString("ssid", WIFI_CFG_DEFAULT_STA_SSID);
  gStaPass = gPrefs.getString("pass", WIFI_CFG_DEFAULT_STA_PASS);
  gApSsid = gPrefs.getString("ap_ssid", WIFI_CFG_DEFAULT_AP_SSID);
  gApPass = gPrefs.getString("ap_pass", WIFI_CFG_DEFAULT_AP_PASS);
  gToken = gPrefs.getString("token", WIFI_CFG_DEFAULT_API_TOKEN);
  gConfigPass = gPrefs.getString("cfg_pass", WIFI_CFG_DEFAULT_CONFIG_PASS);
  const uint32_t storedCycle = gPrefs.getULong("cycle_ms", kDefaultCycleIntervalMs);
  const uint32_t storedMinFreeMb = gPrefs.getULong(
      "min_free_mb", static_cast<uint32_t>(kDefaultMinimumFreeSpace / (1024ULL * 1024ULL)));
  gPrefs.end();
  gCycleIntervalMs = sanitizeCycleMs(storedCycle);
  gMinimumFreeSpace = sanitizeMinFreeBytes(storedMinFreeMb);
}

static void startApConfigPortal() {
  gApMode = true;
  WiFi.mode(WIFI_AP);
  const char *pass = (gApPass.length() >= 8) ? gApPass.c_str() : nullptr;
  WiFi.softAP(gApSsid.c_str(), pass);
  Serial.printf("AP mode active. SSID: %s, IP: %s\n",
                gApSsid.c_str(), WiFi.softAPIP().toString().c_str());
  registerHttpHandlers();
}

static bool connectStaWithTimeout(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(gStaSsid.c_str(), gStaPass.c_str());
  Serial.printf("Connecting to STA SSID \"%s\"\n", gStaSsid.c_str());
  const uint32_t start = millis();
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

static void maintainWifiConnection(uint32_t nowMs) {
  if (gApMode || gStaSsid.isEmpty() || WiFi.status() == WL_CONNECTED) {
    return;
  }
  if (nowMs - gLastStaReconnectMs >= kStaReconnectIntervalMs) {
    gLastStaReconnectMs = nowMs;
    Serial.println("[WIFI] STA disconnected; triggering non-blocking reconnect...");
    WiFi.disconnect();
    WiFi.begin(gStaSsid.c_str(), gStaPass.c_str());
  }
}

// ----------------- Setup & loop -----------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32-S3 CAM + DHT11 logger with HTTP API");
  loadPrefs();

  if (!initSdCard()) {
    Serial.println("SD init failed; starting AP config portal without SD logging");
    startApConfigPortal();
    return;
  }
  gSdReady = true;

  if (!ensureDir("/data")) {
    Serial.println("Failed to create /data; halt");
    return;
  }

  File root = SD_MMC.open("/data");
  uint32_t maxRun = 0;
  if (root) {
    File d = root.openNextFile();
    while (d) {
      if (d.isDirectory()) {
        String name = d.name();
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

  if (!gApMode && !gStaSsid.isEmpty() && connectStaWithTimeout(15000)) {
    Serial.println("Using STA mode");
    registerHttpHandlers();
  } else {
    Serial.println("Falling back to AP config portal");
    startApConfigPortal();
  }

  captureFrameOnce(true);
  recordDhtReadingOnce();
}

void loop() {
  static uint32_t lastCycleMs = 0;
  const uint32_t now = millis();

  maintainWifiConnection(now);

  if (gSdReady && (now - lastCycleMs >= gCycleIntervalMs)) {
    lastCycleMs = now;
    captureFrameOnce(false);
    recordDhtReadingOnce();
  }

  gServer.handleClient();
}
