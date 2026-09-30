#pragma once
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

// ISO14443A UIDs read by this module: 4, 7, or 10 bytes.
// Hex text therefore needs at most 20 characters plus the terminating zero.
constexpr size_t RFID_UID_TEXT_CAPACITY = 21;

enum class RfidScanMode : uint8_t { Disabled, Authentication, Enrollment };
enum class RfidReaderState : uint8_t { Disabled, Starting, Ready, Unavailable };
enum class RfidEventType : uint8_t { CardScanned, ReadError };

struct RfidStatus {
    RfidReaderState state = RfidReaderState::Unavailable;
    uint8_t version = 0; // Chip register only; not proof of RF performance.
};

struct RfidEvent {
    RfidEventType type = RfidEventType::ReadError;
    RfidScanMode mode = RfidScanMode::Disabled;
    uint32_t generation = 0; // Controller operation token, NOT a backend session.
    uint8_t uidLength = 0;   // Byte length, not hex-text length.
    char uid[RFID_UID_TEXT_CAPACITY] = "";
};

static_assert(std::is_trivially_copyable<RfidEvent>::value,
              "RFID queue messages must be byte-copy safe");

// Call once from setup before starting the controller. True means queues/task
// were created, NOT that a physical reader has been verified.
bool beginRFID();

// Controller -> worker. One controller owns these commands. Each new operation
// needs a new nonzero generation. Latest command wins; pending old scans expire.
// Mode Disabled cancels scanning without powering down the entire firmware.
bool setRFIDScanMode(RfidScanMode mode, uint32_t generation);

// Worker -> controller. Non-blocking, task context only. No screen/backend calls
// are made by the RFID worker. The controller must check mode AND generation.
bool pollRFIDEvent(RfidEvent &event);
RfidStatus getRFIDStatus();
