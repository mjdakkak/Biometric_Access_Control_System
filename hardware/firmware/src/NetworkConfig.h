#pragma once
#include <stdint.h>
#include <stddef.h>

#ifndef KIOSK_ENABLE_NETWORK
#define KIOSK_ENABLE_NETWORK 1
#endif

namespace NetworkConfig {
constexpr const char *BASE_URL =
    "https://biometricaccesscontrolsystem-production.up.railway.app";
constexpr uint32_t CONTROLLER_TIMEOUT_MS = 15000;
constexpr uint32_t REQUEST_BUDGET_MS = 13000;
constexpr int SOCKET_TIMEOUT_MS = 4000;
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr size_t MAX_RESPONSE_BYTES = 8192;
constexpr size_t MAX_REQUEST_JSON_BYTES = 768;
constexpr size_t MAX_JPEG_BYTES = 192 * 1024;
constexpr const char *NTP_PRIMARY = "pool.ntp.org";
constexpr const char *NTP_SECONDARY = "time.cloudflare.com";
}
