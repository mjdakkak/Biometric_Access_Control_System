#pragma once
#include "fingerprint.h"

// TRANSPORT-INDEPENDENT maintenance interface. It does not authenticate a caller.
// Never wire these mutation APIs directly to Nextion text, a public HTTP route,
// or an unauthenticated serial console. The future adapter must verify a fresh,
// device-bound backend decision before submitting a plan/final acknowledgement.
// The compile-time KIOSK_ENABLE_FP_RECOVERY_APPLY gate defaults to 0.
// Normal fingerprint commands must be cancelled (operation None) before inspection.
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
    uint32_t requestId=0; // Local maintenance request correlation; not authorization.
    FingerprintRecoveryResult result=FingerprintRecoveryResult::InvalidJournal;
    char ticket[33]="";    // Per-inspection nonce. Correlation only, not a secret.
    char journalId[65]=""; // SHA-256 of the exact persistent record (not authentication).
    FingerprintCommand pending{}; // Original session + exact affected/protected slots.
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
    // No arbitrary slot list: only slots in the validated persistent journal
    // can be affected. The backend must reserve this sensor/these slots until done.
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

// All functions are task-context only. bool return means queued, not completed.
bool requestFingerprintRecoveryInspection(uint32_t requestId);
bool pollFingerprintRecoveryReport(FingerprintRecoveryReport &report);
bool submitFingerprintRecoveryPlan(const FingerprintRecoveryPlan &plan);
bool pollFingerprintRecoveryReceipt(FingerprintRecoveryReceipt &receipt);
// Call ONLY after backend has durably reconciled its database and confirms this
// exact receipt. Worker rechecks sensor state before clearing the NVS record.
bool acknowledgeFingerprintRecovery(const FingerprintRecoveryAcknowledgement &ack);
void cancelFingerprintRecovery(); // Cannot undo a store/delete already transmitted.
