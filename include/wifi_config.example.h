#pragma once

// Copy this file to include/wifi_config.h (git-ignored) to override initial NVS defaults.
// Runtime settings can also be updated via POST /config and stored in ESP32 NVS Preferences.

#define WIFI_CFG_DEFAULT_STA_SSID     ""
#define WIFI_CFG_DEFAULT_STA_PASS     ""
#define WIFI_CFG_DEFAULT_AP_SSID      "ESP32S3-CAM-SETUP"
#define WIFI_CFG_DEFAULT_AP_PASS      ""          // Empty = open onboarding AP until configured in /config
#define WIFI_CFG_DEFAULT_CONFIG_USER  "admin"
#define WIFI_CFG_DEFAULT_CONFIG_PASS  ""          // Set a strong password to require HTTP Basic Auth on /config
#define WIFI_CFG_DEFAULT_API_TOKEN    ""          // Set a token to require X-Auth-Token on /frames and /browse
