#pragma once
#include <stdint.h>

// These are COMMANDED output states, not door/bolt sensor measurements.
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

// Call ONCE from setup, before the screen/controller tasks start.
// true = initialization succeeded (or intentionally disabled); NOT a lock test.
// HardwareConfig.h supplies enable, polarity, duration, and timing settings.
bool initLock(int pin);

// Controller task only, after final authenticated backend approval.
// Non-blocking: true means a pulse was ACCEPTED, not that a door opened.
// Rejects disabled/uninitialized/faulted/busy/cooldown/invalid-duration calls.
// A repeated call NEVER extends the current pulse or queues another pulse.
bool triggerLock(int durationMS);

// Cancel a pending pulse and command the locked level now. Task context only.
// Uses a short bounded mutex wait; check false as a control failure.
// Cannot guarantee mechanical locking, safe egress, or power-failure behavior.
bool lockNow();

// Non-blocking snapshot. Does not read a physical door or bolt sensor.
LockStatus getLockStatus();
