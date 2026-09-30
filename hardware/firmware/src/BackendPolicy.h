#pragma once
#include <stdint.h>

// Backend policy; sensor addresses are checked separately.
namespace BackendPolicy {
constexpr int FINGERPRINT_SLOT_MIN = 1;
constexpr int FINGERPRINT_SLOT_MAX = 162;
constexpr uint32_t AUTH_SESSION_MS = 60UL * 1000UL;
constexpr uint32_t ENROLL_SESSION_MS = 300UL * 1000UL;

constexpr bool validFingerprintSlot(int slot) {
    return slot >= FINGERPRINT_SLOT_MIN && slot <= FINGERPRINT_SLOT_MAX;
}
constexpr bool validFingerprintMatch(int slot) {
    return slot == -1 || validFingerprintSlot(slot); // -1 is sent as JSON null.
}

// Unsigned subtraction handles millis() rollover.
inline bool elapsedAtLeast(uint32_t now, uint32_t start, uint32_t duration) {
    return static_cast<uint32_t>(now - start) >= duration;
}

struct SessionDeadline {
    uint32_t startedMs = 0;
    uint32_t lifetimeMs = 0; // Zero means no active session.
    bool active() const { return lifetimeMs != 0; }
    void clear() { startedMs = lifetimeMs = 0; }
    bool begin(bool authentication, uint32_t initialRequestQueuedMs) {
        if (active()) return false; // Do not extend an active session.
        startedMs = initialRequestQueuedMs;
        lifetimeMs = authentication ? AUTH_SESSION_MS : ENROLL_SESSION_MS;
        return true;
    }
    bool expired(uint32_t now) const {
        return active() && elapsedAtLeast(now, startedMs, lifetimeMs);
    }
    uint32_t remainingMs(uint32_t now) const {
        if (!active() || expired(now)) return 0;
        return lifetimeMs - static_cast<uint32_t>(now - startedMs);
    }
};
}
