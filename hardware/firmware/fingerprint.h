#pragma once
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

// Templates stay in the sensor; queues carry slot numbers and status.
constexpr size_t FP_SESSION_CAPACITY = 97;
constexpr size_t FP_MAX_SLOTS = 2;

enum class FingerprintOperation : uint8_t { None, Match, Enroll, DeleteOld };
enum class FingerprintReaderState : uint8_t {
    Disabled, Starting, Ready, Unavailable, RecoveryRequired
};
enum class FingerprintEventType : uint8_t {
    Progress, Matched, NoMatch, Enrolled, OldDeleted, Failed
};
enum class FingerprintPrompt : uint8_t {
    RemoveFinger, PlaceFinger, PlaceSameFinger, Processing,
    Storing, Deleting, PoorImage, UseDifferentFinger
};
enum class FingerprintError : uint8_t {
    None, Disabled, Unavailable, InvalidCommand, InvalidSlot,
    SlotOccupied, IndexUnavailable, BadImage, SamplesDiffer,
    DuplicateFinger, TimedOut, SensorCommunication, SensorRejected,
    StorageFailed, DeleteFailed, JournalFailed, RecoveryRequired
};

struct FingerprintCommand {
    FingerprintOperation operation = FingerprintOperation::None;
    uint32_t generation = 0; // Hardware-operation generation, not an HTTP request ID.
    char sessionId[FP_SESSION_CAPACITY] = "";
    int targetSlot = -1; // Assigned by the backend.
    uint8_t deleteCount = 0;
    int deleteSlots[FP_MAX_SLOTS] = {-1, -1};
    // Replacements already confirmed in this session; both are required before deletion.
    uint8_t protectedCount = 0;
    int protectedSlots[FP_MAX_SLOTS] = {-1, -1};
};
struct FingerprintEvent {
    FingerprintEventType type = FingerprintEventType::Failed;
    FingerprintOperation operation = FingerprintOperation::None;
    uint32_t generation = 0;
    FingerprintPrompt prompt = FingerprintPrompt::Processing;
    FingerprintError error = FingerprintError::None;
    uint8_t sensorCode = 0;
    int slot = -1;
    uint16_t confidence = 0; // Diagnostic score, not an unlock decision.
    uint8_t deletedCount = 0;
};
struct FingerprintStatus {
    FingerprintReaderState state = FingerprintReaderState::Unavailable;
    uint16_t capacity = 0;
    bool mutationPending = false; // Journal is waiting for server confirmation.
};
static_assert(std::is_trivially_copyable<FingerprintCommand>::value, "Queue safety");
static_assert(std::is_trivially_copyable<FingerprintEvent>::value, "Queue safety");

// Creates the worker and queues; sensor readiness is reported separately.
bool beginFingerprint();
// Latest generation wins. Cancellation cannot undo a sent store/delete command.
bool requestFingerprint(const FingerprintCommand &command);
bool pollFingerprintEvent(FingerprintEvent &event);
FingerprintStatus getFingerprintStatus();
// Queue acknowledgement only for the matching successful backend operation.
bool acknowledgeFingerprintMutation(uint32_t generation,
                                     FingerprintOperation operation,
                                     const char *sessionId);
const char *fingerprintErrorText(FingerprintError error);
