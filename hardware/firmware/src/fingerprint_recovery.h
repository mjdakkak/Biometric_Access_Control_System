#pragma once
#include "fingerprint.h"

// A trusted adapter must authorize repairs; this interface does not authenticate callers.
// Recovery apply is disabled by default. Cancel normal work before inspection.
enum class FingerprintSlotObservation : uint8_t { Unknown, Absent, PresentReadable, PresentUnreadable };
enum class FingerprintRecoveryAction : uint8_t {
    Hold, KeepStoredTemplate, DiscardUncommittedTemplate, FinishOldDeletion
};
enum class FingerprintRecoveryResult : uint8_t {
    Inspected, AppliedAwaitingBackend, Cleared, NoPending, Busy, Disabled,
    ApplyDisabled, InvalidJournal, InvalidDecision, StaleDecision,
    SensorUnavailable, SensorChanged, VerificationFailed, StorageFailed,
    TimedOut, Cancelled
};
struct FingerprintRecoveryReport {
    uint32_t requestId=0;
    FingerprintRecoveryResult result=FingerprintRecoveryResult::InvalidJournal;
    char ticket[33]="";    // Matches a plan to its inspection.
    char journalId[65]=""; // Record hash, not authorization.
    FingerprintCommand pending{}; // Original affected and protected slots.
    uint16_t sensorCapacity=0;
    FingerprintSlotObservation target=FingerprintSlotObservation::Unknown;
    FingerprintSlotObservation oldSlots[FP_MAX_SLOTS]={};
    FingerprintSlotObservation protectedSlots[FP_MAX_SLOTS]={};
};
struct FingerprintRecoveryPlan {
    uint32_t requestId=0;
    char ticket[33]="";
    char journalId[65]="";
    FingerprintRecoveryAction action=FingerprintRecoveryAction::Hold;
    // Plans act only on the recorded slots; the backend reserves them during repair.
};
struct FingerprintRecoveryReceipt {
    uint32_t requestId=0;
    FingerprintRecoveryResult result=FingerprintRecoveryResult::VerificationFailed;
    FingerprintRecoveryAction action=FingerprintRecoveryAction::Hold;
    char ticket[33]="";
    char journalId[65]="";
    char sessionId[FP_SESSION_CAPACITY]="";
    uint8_t removedThisAttempt=0;
};
struct FingerprintRecoveryAcknowledgement {
    uint32_t requestId=0;
    char receiptTicket[33]="";
    char journalId[65]="";
    char sessionId[FP_SESSION_CAPACITY]="";
};
static_assert(std::is_trivially_copyable<FingerprintRecoveryReport>::value,"Queue safety");
static_assert(std::is_trivially_copyable<FingerprintRecoveryPlan>::value,"Queue safety");
static_assert(std::is_trivially_copyable<FingerprintRecoveryReceipt>::value,"Queue safety");

// Task context only. True means queued, not completed.
bool requestFingerprintRecoveryInspection(uint32_t requestId);
bool pollFingerprintRecoveryReport(FingerprintRecoveryReport &report);
bool submitFingerprintRecoveryPlan(const FingerprintRecoveryPlan &plan);
bool pollFingerprintRecoveryReceipt(FingerprintRecoveryReceipt &receipt);
// Clear only after the backend commits this receipt and the sensor state is rechecked.
bool acknowledgeFingerprintRecovery(const FingerprintRecoveryAcknowledgement &ack);
void cancelFingerprintRecovery(); // Cannot undo a sent write.
