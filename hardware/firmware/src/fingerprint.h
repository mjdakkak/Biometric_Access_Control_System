#pragma once
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

// These lengths match BackendInterface.h. No biometric templates/images are
// passed through a queue: the sensor keeps them; firmware reports numeric slots.
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
    uint32_t generation = 0; // Hardware-operation token, NOT HTTP request ID.
    char sessionId[FP_SESSION_CAPACITY] = "";
    int targetSlot = -1; // Must come from the backend for Enroll.
    uint8_t deleteCount = 0;
    int deleteSlots[FP_MAX_SLOTS] = {-1, -1};
    // New templates already acknowledged by backend IN THIS SESSION.
    // Enroll: 0 for first finger, 1 for second. DeleteOld: exactly 2.
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
    uint16_t confidence = 0; // Diagnostic only, not a probability or unlock decision.
    uint8_t deletedCount = 0;
};
struct FingerprintStatus {
    FingerprintReaderState state = FingerprintReaderState::Unavailable;
    uint16_t capacity = 0;
    bool mutationPending = false; // Persistent journal awaits server acknowledgement.
};
static_assert(std::is_trivially_copyable<FingerprintCommand>::value, "Queue safety");
static_assert(std::is_trivially_copyable<FingerprintEvent>::value, "Queue safety");

// Called once from setup. True means task/queues created, not sensor verified.
bool beginFingerprint();
// One controller owns commands. Latest generation wins; None cancels capture.
// Cancellation cannot undo a store/delete command already sent to the sensor.
bool requestFingerprint(const FingerprintCommand &command);
bool pollFingerprintEvent(FingerprintEvent &event);
FingerprintStatus getFingerprintStatus();
// Call ONLY after a validated, correlated successful backend response for the
// exact Enroll/DeleteOld operation. Returns true if queued, not if NVS cleared.
// A separate acknowledgement queue prevents a new capture overwriting this ack.
bool acknowledgeFingerprintMutation(uint32_t generation,
                                     FingerprintOperation operation,
                                     const char *sessionId);
const char *fingerprintErrorText(FingerprintError error);
