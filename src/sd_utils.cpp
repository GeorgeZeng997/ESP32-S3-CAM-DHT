#include "sd_utils.h"

static const int kSdClkPin = 39;
static const int kSdCmdPin = 38;
static const int kSdData0Pin = 40;

bool initSdCard() {
  SD_MMC.setPins(kSdClkPin, kSdCmdPin, kSdData0Pin);
  // Lower init frequency to improve stability with some cards/modules.
  if (!SD_MMC.begin("/sdcard", true, true, SDMMC_FREQ_PROBING, 5)) {
    Serial.println("Card mount failed");
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD_MMC card attached");
    return false;
  }

  Serial.print("SD_MMC Card Type: ");
  switch (cardType) {
    case CARD_MMC: Serial.println("MMC"); break;
    case CARD_SD: Serial.println("SDSC"); break;
    case CARD_SDHC: Serial.println("SDHC"); break;
    default: Serial.println("UNKNOWN"); break;
  }

  Serial.printf("Card size: %lluMB\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
  Serial.printf("Total space: %lluMB\n", SD_MMC.totalBytes() / (1024ULL * 1024ULL));
  Serial.printf("Used space: %lluMB\n", SD_MMC.usedBytes() / (1024ULL * 1024ULL));
  return true;
}

bool ensureDir(const char *path) {
  if (SD_MMC.exists(path)) {
    return true;
  }
  bool created = SD_MMC.mkdir(path);
  if (!created) {
    Serial.printf("Failed to create dir: %s\n", path);
  }
  return created;
}

uint64_t sdFreeBytes() {
  uint64_t total = SD_MMC.totalBytes();
  uint64_t used = SD_MMC.usedBytes();
  return (total > used) ? (total - used) : 0;
}

bool saveJpegFrame(const char *dirPath, uint32_t frameIndex, const uint8_t *data, size_t len, String &savedPath) {
  char path[96];
  snprintf(path, sizeof(path), "%s/frame_%06lu.jpg", dirPath, (unsigned long)frameIndex);

  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("Failed to open %s for write\n", path);
    return false;
  }

  size_t written = file.write(data, len);
  file.close();

  if (written != len) {
    Serial.printf("Write incomplete (%u/%u)\n", (unsigned)written, (unsigned)len);
    return false;
  }

  savedPath = path;
  return true;
}
