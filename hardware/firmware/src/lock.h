#pragma once
#include <stdint.h>

// Commanded output state, not physical door feedback.
enum class LockState : uint8_t {
    NotInitialized, Disabled, LockedOutput, PendingUnlock,
    UnlockOutput, Cooldown, Fault
};

enum class LockError : uint8_t {
    None, NotConfigured, UnsafePin, ResourceFailure, GpioFailure, StartExpired
};

struct LockStatus {
    LockState state = LockState::NotInitialized;
    LockError error = LockError::None;
    uint32_t pulseNumber = 0;
    bool pulseStarted = false;
};

// Initialize once before starting the controller.
bool initLock(int pin);

// Controller only, after final authentication approval.
// Accept one non-blocking pulse; repeated requests do not extend it.
bool triggerLock(int durationMS);

// Cancel a pending pulse and request the locked level. Check the return value.
bool lockNow();

// Returns a status snapshot, not a door-sensor reading.
LockStatus getLockStatus();
