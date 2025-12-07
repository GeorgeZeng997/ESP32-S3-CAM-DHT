#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>

// Initializes SD_MMC. Returns true on success.
bool initSdCard();

// Ensures the directory exists (creates it if missing).
bool ensureDir(const char *path);

// Remaining usable space on the card.
uint64_t sdFreeBytes();

// Saves a JPEG frame into the given directory using an incremental filename.
// Returns true on success and fills savedPath with the written location.
bool saveJpegFrame(const char *dirPath, uint32_t frameIndex, const uint8_t *data, size_t len, String &savedPath);
