#pragma once
#include <stdint.h>

// Keep 0 while writing firmware before building the breadboard.
// The real RFID driver STILL COMPILES; 0 only prevents SPI/reader startup.
// This does not generate fake scans or approve credentials.
// Later, after checking the complete pin map and wiring, change 0 to 1.
#ifndef KIOSK_ENABLE_RFID
#define KIOSK_ENABLE_RFID 1
#endif

// 0 = compile the AS608 worker, but do not open its UART or NVS journal.
// Do not enable either reader until the full pin map and wiring are checked.
#ifndef KIOSK_ENABLE_FINGERPRINT
#define KIOSK_ENABLE_FINGERPRINT 1
#endif

// 0 = compile the camera worker without initializing/capturing from hardware.
#ifndef KIOSK_ENABLE_CAMERA
#define KIOSK_ENABLE_CAMERA 1
#endif

// Lock driver intentionally unassigned until the real driver/lock is identified.
#ifndef KIOSK_ENABLE_LOCK
#define KIOSK_ENABLE_LOCK 0
#endif
#ifndef KIOSK_LOCK_PIN
#define KIOSK_LOCK_PIN 14
#endif
#ifndef KIOSK_LOCK_UNLOCK_LEVEL
#define KIOSK_LOCK_UNLOCK_LEVEL -1
#endif


// Logic enabled, physical camera still disabled above. Never fall back to an
// uncropped frame when local processing is unavailable or rejects an image.
#ifndef KIOSK_ENABLE_FACE_PROCESSING
#define KIOSK_ENABLE_FACE_PROCESSING 1
#endif
// Leave 0 until a trusted maintenance/backend adapter authenticates decisions.
// This gate is NOT authentication by itself. Inspection is read-only.
#ifndef KIOSK_ENABLE_FP_RECOVERY_APPLY
#define KIOSK_ENABLE_FP_RECOVERY_APPLY 0
#endif

namespace HardwareConfig {
constexpr bool FACE_PROCESSING_ENABLED=(KIOSK_ENABLE_FACE_PROCESSING!=0);
constexpr float FACE_SCORE_THRESHOLD=0.5F; // Starting threshold, not calibrated identity confidence.
constexpr unsigned FACE_MIN_SIDE_PX=96;  // In full VGA image; tune after physical validation.
constexpr unsigned FACE_CROP_MARGIN_PERCENT=25; // Per side, clamped to original frame.
constexpr uint8_t FACE_JPEG_QUALITY=90; // Converter quality (1..100), not sensor quality (0..63).
constexpr bool FP_RECOVERY_APPLY_ENABLED=(KIOSK_ENABLE_FP_RECOVERY_APPLY!=0);
constexpr uint32_t FP_RECOVERY_IO_TIMEOUT_MS=20000;
constexpr uint32_t FP_RECOVERY_PLAN_TTL_MS=60000;

constexpr bool LOCK_ENABLED = (KIOSK_ENABLE_LOCK != 0);
constexpr int LOCK_PIN = KIOSK_LOCK_PIN;
// GPIO logic level that commands UNLOCK, not relay-coil active polarity alone.
// -1 = unknown; choose 0 or 1 only after checking driver + contact + lock behavior.
constexpr int LOCK_UNLOCK_LEVEL = KIOSK_LOCK_UNLOCK_LEVEL;
// Prototype policy values, NOT a solenoid thermal/duty-cycle rating.
constexpr uint32_t LOCK_PULSE_MS = 3000;
constexpr uint32_t LOCK_MAX_PULSE_MS = 5000;
constexpr uint32_t LOCK_COOLDOWN_MS = 1000;
constexpr uint32_t LOCK_SERVICE_MS = 10;
constexpr uint32_t LOCK_START_TTL_MS = 250;

constexpr bool CAMERA_ENABLED = (KIOSK_ENABLE_CAMERA != 0);
// Freenove ESP32-S3 WROOM camera wiring, matching the original sketch.
// Do not move these camera signals to arbitrary free pins on the board.
constexpr int8_t CAMERA_PWDN_PIN=-1, CAMERA_RESET_PIN=-1;
constexpr int8_t CAMERA_XCLK_PIN=15, CAMERA_SDA_PIN=4, CAMERA_SCL_PIN=5;
constexpr int8_t CAMERA_D0_PIN=11, CAMERA_D1_PIN=9, CAMERA_D2_PIN=8, CAMERA_D3_PIN=10;
constexpr int8_t CAMERA_D4_PIN=12, CAMERA_D5_PIN=18, CAMERA_D6_PIN=17, CAMERA_D7_PIN=16;
constexpr int8_t CAMERA_VSYNC_PIN=6, CAMERA_HREF_PIN=7, CAMERA_PCLK_PIN=13;
// Reserve LEDC channel 0 / timer 0 for the camera clock (not for lock/LED PWM).
constexpr bool CAMERA_REQUIRE_PSRAM=true;
constexpr uint32_t CAMERA_POLL_MS=20;
constexpr uint32_t CAMERA_POSITION_DELAY_MS=3000;
constexpr uint32_t CAMERA_CAPTURE_TIMEOUT_MS=8000;
constexpr uint32_t CAMERA_MAX_FRAME_AGE_MS=1500;
constexpr uint32_t CAMERA_IMAGE_TTL_MS=20000; // Only unborrowed copies expire.
constexpr unsigned CAMERA_WARMUP_FRAMES=3;
constexpr unsigned CAMERA_IMAGE_POOL_SIZE=2;
constexpr unsigned CAMERA_MAX_JPEG_BYTES=192U*1024U;

constexpr bool FINGERPRINT_ENABLED = (KIOSK_ENABLE_FINGERPRINT != 0);
// Carried forward from YOUR ORIGINAL sketch. ESP RX connects to sensor TX.
// GPIO3 is an ESP32-S3 strapping pin: review before wiring/final PCB allocation.
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
constexpr uint32_t FINGER_ENROLL_TIMEOUT_MS = 60000; // Whole two-capture operation.
constexpr uint32_t FINGER_DELETE_TIMEOUT_MS = 15000;
constexpr uint32_t FINGER_REMOVAL_MS = 300;
constexpr uint8_t FINGER_MAX_BAD_CAPTURES = 3;
// Defensive firmware limit; actual range is read from the sensor at startup.
constexpr uint16_t FINGER_MAX_SUPPORTED_CAPACITY = 4096;

constexpr bool RFID_ENABLED = (KIOSK_ENABLE_RFID != 0);

// Kept from the last screen package. These were proposed, not board-verified.
constexpr int8_t NEXTION_RX_PIN = 39; // Nextion TX -> ESP32 input
constexpr int8_t NEXTION_TX_PIN = 40; // Nextion RX <- ESP32 output
constexpr uint32_t NEXTION_BAUD = 115200;

// Kept from YOUR ORIGINAL sketch; not a final PCB pin allocation.
// The MFRC522 library uses the global SPI object. This worker owns it for now.
// Do not initialize that same SPI bus elsewhere without coordinating ownership.
constexpr int8_t RFID_SS_PIN   = 21;
constexpr int8_t RFID_RST_PIN  = 47;
constexpr int8_t RFID_SCK_PIN  = 41;
constexpr int8_t RFID_MISO_PIN = 42;
constexpr int8_t RFID_MOSI_PIN = 2;

constexpr uint32_t RFID_POLL_MS = 75;
constexpr uint32_t RFID_RETRY_MS = 3000;
constexpr uint32_t RFID_REMOVAL_MS = 500;

// Conservative compile-time check: this only covers the pins above, not your
// board circuitry, USB/JTAG usage, external circuitry, or flash/PSRAM wiring.
// The lock module checks its selected pin AGAINST this non-lock pin list.
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
