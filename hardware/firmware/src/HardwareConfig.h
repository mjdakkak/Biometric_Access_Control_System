#pragma once
#include <stdint.h>

// Enable each peripheral after checking its wiring.
#ifndef KIOSK_ENABLE_RFID
#define KIOSK_ENABLE_RFID 1
#endif

#ifndef KIOSK_ENABLE_FINGERPRINT
#define KIOSK_ENABLE_FINGERPRINT 1
#endif

#ifndef KIOSK_ENABLE_CAMERA
#define KIOSK_ENABLE_CAMERA 1
#endif

// Lock polarity must match the verified driver circuit.
#ifndef KIOSK_ENABLE_LOCK
#define KIOSK_ENABLE_LOCK 0
#endif
#ifndef KIOSK_LOCK_PIN
#define KIOSK_LOCK_PIN 14
#endif
#ifndef KIOSK_LOCK_UNLOCK_LEVEL
#define KIOSK_LOCK_UNLOCK_LEVEL -1
#endif

// Never upload a full frame when local face processing rejects it.
#ifndef KIOSK_ENABLE_FACE_PROCESSING
#define KIOSK_ENABLE_FACE_PROCESSING 1
#endif
// Keep recovery apply off until the authorized backend exchange is ready.
#ifndef KIOSK_ENABLE_FP_RECOVERY_APPLY
#define KIOSK_ENABLE_FP_RECOVERY_APPLY 0
#endif

namespace HardwareConfig {
constexpr bool FACE_PROCESSING_ENABLED=(KIOSK_ENABLE_FACE_PROCESSING!=0);
constexpr float FACE_SCORE_THRESHOLD=0.5F; // Detection threshold, not match confidence.
constexpr unsigned FACE_MIN_SIDE_PX=96;  // Original-frame pixels.
constexpr unsigned FACE_CROP_MARGIN_PERCENT=25; // Per-side padding, clipped to the frame.
constexpr uint8_t FACE_JPEG_QUALITY=90; // Re-encoded JPEG quality: 1-100.
constexpr bool FP_RECOVERY_APPLY_ENABLED=(KIOSK_ENABLE_FP_RECOVERY_APPLY!=0);
constexpr uint32_t FP_RECOVERY_IO_TIMEOUT_MS=20000;
constexpr uint32_t FP_RECOVERY_PLAN_TTL_MS=60000;

constexpr bool LOCK_ENABLED = (KIOSK_ENABLE_LOCK != 0);
constexpr int LOCK_PIN = KIOSK_LOCK_PIN;
// Unlock GPIO level; -1 means unconfigured.
constexpr int LOCK_UNLOCK_LEVEL = KIOSK_LOCK_UNLOCK_LEVEL;
// Pulse limits must also suit the physical lock.
constexpr uint32_t LOCK_PULSE_MS = 3000;
constexpr uint32_t LOCK_MAX_PULSE_MS = 5000;
constexpr uint32_t LOCK_COOLDOWN_MS = 1000;
constexpr uint32_t LOCK_SERVICE_MS = 10;
constexpr uint32_t LOCK_START_TTL_MS = 250;

constexpr bool CAMERA_ENABLED = (KIOSK_ENABLE_CAMERA != 0);
// Fixed camera connections on the Freenove board.
constexpr int8_t CAMERA_PWDN_PIN=-1, CAMERA_RESET_PIN=-1;
constexpr int8_t CAMERA_XCLK_PIN=15, CAMERA_SDA_PIN=4, CAMERA_SCL_PIN=5;
constexpr int8_t CAMERA_D0_PIN=11, CAMERA_D1_PIN=9, CAMERA_D2_PIN=8, CAMERA_D3_PIN=10;
constexpr int8_t CAMERA_D4_PIN=12, CAMERA_D5_PIN=18, CAMERA_D6_PIN=17, CAMERA_D7_PIN=16;
constexpr int8_t CAMERA_VSYNC_PIN=6, CAMERA_HREF_PIN=7, CAMERA_PCLK_PIN=13;
// LEDC channel 0 / timer 0 belongs to the camera clock.
constexpr bool CAMERA_REQUIRE_PSRAM=true;
constexpr uint32_t CAMERA_POLL_MS=20;
constexpr uint32_t CAMERA_POSITION_DELAY_MS=3000;
constexpr uint32_t CAMERA_CAPTURE_TIMEOUT_MS=8000;
constexpr uint32_t CAMERA_MAX_FRAME_AGE_MS=1500;
constexpr uint32_t CAMERA_IMAGE_TTL_MS=20000; // Borrowed images stay valid until release.
constexpr unsigned CAMERA_WARMUP_FRAMES=3;
constexpr unsigned CAMERA_IMAGE_POOL_SIZE=2;
constexpr unsigned CAMERA_MAX_JPEG_BYTES=192U*1024U;

constexpr bool FINGERPRINT_ENABLED = (KIOSK_ENABLE_FINGERPRINT != 0);
// UART pins are from the ESP32 perspective; cross RX/TX at the sensor.
constexpr int8_t FINGER_RX_PIN = 1;
constexpr int8_t FINGER_TX_PIN = 38;
constexpr uint32_t FINGER_BAUD = 57600;
constexpr uint32_t FINGER_PASSWORD = 0x00000000;
constexpr uint32_t FINGER_ADDRESS = 0xFFFFFFFF;
constexpr uint32_t FINGER_POLL_MS = 60;
constexpr uint32_t FINGER_BOOT_MS = 1000;
constexpr uint32_t FINGER_RETRY_MS = 3000;
constexpr uint32_t FINGER_COMMAND_TIMEOUT_MS = 1200;
constexpr uint32_t FINGER_MATCH_TIMEOUT_MS = 20000;
constexpr uint32_t FINGER_ENROLL_TIMEOUT_MS = 60000; // Both captures share this deadline.
constexpr uint32_t FINGER_DELETE_TIMEOUT_MS = 15000;
constexpr uint32_t FINGER_REMOVAL_MS = 300;
constexpr uint8_t FINGER_MAX_BAD_CAPTURES = 3;
// Upper bound; actual capacity is read from the sensor.
constexpr uint16_t FINGER_MAX_SUPPORTED_CAPACITY = 4096;

constexpr bool RFID_ENABLED = (KIOSK_ENABLE_RFID != 0);

// GPIO38/39/40 share the SD interface; leave it unused.
constexpr int8_t NEXTION_RX_PIN = 39; // Nextion TX -> ESP32 RX
constexpr int8_t NEXTION_TX_PIN = 40; // Nextion RX <- ESP32 TX
constexpr uint32_t NEXTION_BAUD = 115200;

// The RFID worker owns the global SPI bus.
constexpr int8_t RFID_SS_PIN   = 21;
constexpr int8_t RFID_RST_PIN  = 47;
constexpr int8_t RFID_SCK_PIN  = 41;
constexpr int8_t RFID_MISO_PIN = 42;
constexpr int8_t RFID_MOSI_PIN = 2;

constexpr uint32_t RFID_POLL_MS = 75;
constexpr uint32_t RFID_RETRY_MS = 3000;
constexpr uint32_t RFID_REMOVAL_MS = 500;

// Check software pin conflicts; board wiring still needs validation.
constexpr bool distinctPins(const int8_t *p, unsigned n,
                            unsigned i = 0, unsigned j = 1) {
    return i >= n ? true :
           j >= n ? distinctPins(p, n, i + 1, i + 2) :
           (p[i] != p[j] && distinctPins(p, n, i, j + 1));
}
constexpr int8_t configuredPins[] = {
    NEXTION_RX_PIN, NEXTION_TX_PIN, RFID_SS_PIN, RFID_RST_PIN,
    RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, FINGER_RX_PIN, FINGER_TX_PIN,
    CAMERA_XCLK_PIN, CAMERA_SDA_PIN, CAMERA_SCL_PIN,
    CAMERA_D0_PIN, CAMERA_D1_PIN, CAMERA_D2_PIN, CAMERA_D3_PIN,
    CAMERA_D4_PIN, CAMERA_D5_PIN, CAMERA_D6_PIN, CAMERA_D7_PIN,
    CAMERA_VSYNC_PIN, CAMERA_HREF_PIN, CAMERA_PCLK_PIN
};
static_assert(distinctPins(configuredPins, sizeof(configuredPins) / sizeof(configuredPins[0])),
              "Nextion, RFID, fingerprint, and camera signals must not overlap");
}
