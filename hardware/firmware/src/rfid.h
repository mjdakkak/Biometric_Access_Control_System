#pragma once
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

// UIDs are 4, 7 or 10 bytes, encoded as uppercase hex.
constexpr size_t RFID_UID_TEXT_CAPACITY = 21;

enum class RfidScanMode : uint8_t { Disabled, Authentication, Enrollment };
enum class RfidReaderState : uint8_t { Disabled, Starting, Ready, Unavailable };
enum class RfidEventType : uint8_t { CardScanned, ReadError };

struct RfidStatus {
    RfidReaderState state = RfidReaderState::Unavailable;
    uint8_t version = 0;
};

struct RfidEvent {
    RfidEventType type = RfidEventType::ReadError;
    RfidScanMode mode = RfidScanMode::Disabled;
    uint32_t generation = 0; // Controller operation generation.
    uint8_t uidLength = 0;   // Bytes, not hex characters.
    char uid[RFID_UID_TEXT_CAPACITY] = "";
};

static_assert(std::is_trivially_copyable<RfidEvent>::value,
              "RFID queue messages must be byte-copy safe");

// Start the worker; physical reader status is reported separately.
bool beginRFID();

// Latest mode/generation wins. Disabled cancels scanning.
bool setRFIDScanMode(RfidScanMode mode, uint32_t generation);

// Non-blocking event read; the controller checks mode and generation.
bool pollRFIDEvent(RfidEvent &event);
RfidStatus getRFIDStatus();
