#include "lock.h"
#include "HardwareConfig.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <type_traits>
#include <atomic>

// The mutex protects GPIO writes and pulse state.
namespace lock_detail {
using namespace HardwareConfig;
static_assert(std::is_trivially_copyable<LockStatus>::value,
              "Lock snapshots must be byte-copy safe");
static_assert(LOCK_PULSE_MS > 0 && LOCK_PULSE_MS <= LOCK_MAX_PULSE_MS,
              "Default lock duration exceeds configured maximum");
static_assert(LOCK_MAX_PULSE_MS <= 60000, "Lock pulse guard must remain bounded");
static_assert(LOCK_SERVICE_MS > 0 && LOCK_START_TTL_MS > LOCK_SERVICE_MS,
              "Invalid lock service/start timing");

SemaphoreHandle_t mutex = nullptr;
QueueHandle_t statusQueue = nullptr;
TaskHandle_t taskHandle = nullptr;
bool initAttempted = false;
bool gpioConfigured = false;
int outputPin = -1;
LockStatus status{};
int64_t acceptedUs = 0, unlockUntilUs = 0, cooldownUntilUs = 0;
uint32_t pendingDurationMs = 0;
// Preserve cancellation even when the mutex is busy.
std::atomic<uint32_t> cancelEpoch{0};
uint32_t handledCancelEpoch = 0;

TickType_t ticksAtLeastOne(uint32_t ms) {
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks ? ticks : 1;
}

void publish() {
    if (statusQueue) xQueueOverwrite(statusQueue, &status);
}

bool allowedPin(int pin) {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) return false;
    // Exclude boot straps, USB, memory and default UART0 pins.
    if (pin == 0 || pin == 3 || pin == 19 || pin == 20 ||
        (pin >= 26 && pin <= 37) || (pin >= 43 && pin <= 46)) return false;
    for (unsigned i = 0; i < sizeof(configuredPins)/sizeof(configuredPins[0]); ++i)
        if (pin == configuredPins[i]) return false;
    return true;
}

uint32_t lockedLevel() { return LOCK_UNLOCK_LEVEL == 1 ? 0U : 1U; }

bool drive(bool unlock) {
    if (!gpioConfigured) return false;
    const uint32_t level = unlock ? static_cast<uint32_t>(LOCK_UNLOCK_LEVEL) : lockedLevel();
    return gpio_set_level(static_cast<gpio_num_t>(outputPin), level) == ESP_OK;
}

void latchFault(LockError error) {
    if (gpioConfigured) drive(false);
    pendingDurationMs = 0;
    unlockUntilUs = 0;
    status.state = LockState::Fault;
    status.error = error;
    publish();
}

bool cancelLocked(uint32_t epoch) {
    const bool wasActive = status.state == LockState::UnlockOutput;
    const bool wasPending = status.state == LockState::PendingUnlock;
    const bool ok = drive(false);
    if (!ok) {
        latchFault(LockError::GpioFailure);
    } else if (status.state != LockState::Fault) {
        pendingDurationMs = 0;
        unlockUntilUs = 0;
        if (wasActive) {
            cooldownUntilUs = esp_timer_get_time() + static_cast<int64_t>(LOCK_COOLDOWN_MS)*1000;
            status.state = LockState::Cooldown;
        } else if (wasPending) {
            status.state = LockState::LockedOutput;
        }
        publish();
    }
    handledCancelEpoch = epoch;
    return ok;
}

void serviceLocked(int64_t now) {
    const uint32_t epoch = cancelEpoch.load();
    if (epoch != handledCancelEpoch) {
        cancelLocked(epoch);
        return;
    }
    if (status.state == LockState::Fault) {
        if (gpioConfigured) drive(false);
        return;
    }
    if (status.state == LockState::PendingUnlock) {
        // Never start an old unlock request after a scheduler delay.
        if (now - acceptedUs >= static_cast<int64_t>(LOCK_START_TTL_MS)*1000) {
            latchFault(LockError::StartExpired);
            return;
        }
        // Start the deadline before driving the output.
        unlockUntilUs = now + static_cast<int64_t>(pendingDurationMs)*1000;
        if (!drive(true)) { latchFault(LockError::GpioFailure); return; }
        status.state = LockState::UnlockOutput;
        status.pulseStarted = true;
        publish();
        return;
    }
    if (status.state == LockState::UnlockOutput && now >= unlockUntilUs) {
        if (!drive(false)) { latchFault(LockError::GpioFailure); return; }
        cooldownUntilUs = esp_timer_get_time() + static_cast<int64_t>(LOCK_COOLDOWN_MS)*1000;
        status.state = LockState::Cooldown;
        pendingDurationMs = 0;
        publish();
    }
    if (status.state == LockState::Cooldown && now >= cooldownUntilUs) {
        status.state = LockState::LockedOutput;
        publish();
    }
}

void tickWorker() {
    if (!mutex || xSemaphoreTake(mutex, 0) != pdTRUE) return;
    serviceLocked(esp_timer_get_time());
    xSemaphoreGive(mutex);
}

void worker(void *) {
    while (true) {
        tickWorker();
        vTaskDelay(ticksAtLeastOne(LOCK_SERVICE_MS));
    }
}
}

bool initLock(int pin) {
    using namespace lock_detail;
    if (initAttempted) return false;
    initAttempted = true;
    statusQueue = xQueueCreate(1, sizeof(LockStatus));
    if (!statusQueue) { status.state=LockState::Fault; status.error=LockError::ResourceFailure; return false; }
    if (!HardwareConfig::LOCK_ENABLED) {
        status.state = LockState::Disabled;
        publish();
        return true; // Disabled builds leave the lock pin untouched.
    }
    if (pin < 0 || (HardwareConfig::LOCK_UNLOCK_LEVEL != 0 && HardwareConfig::LOCK_UNLOCK_LEVEL != 1)) {
        latchFault(LockError::NotConfigured);
        return false;
    }
    if (!allowedPin(pin)) { latchFault(LockError::UnsafePin); return false; }
    outputPin = pin;

    // Set the locked level before enabling output. External bias covers boot/reset.
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_DISABLE;
    cfg.pull_up_en = lockedLevel() ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = lockedLevel() ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK ||
        gpio_set_level(static_cast<gpio_num_t>(pin), lockedLevel()) != ESP_OK) {
        latchFault(LockError::GpioFailure);
        return false;
    }
    gpioConfigured = true;
    if (gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT) != ESP_OK) {
        latchFault(LockError::GpioFailure);
        return false;
    }
    mutex = xSemaphoreCreateMutex();
    if (!mutex) { latchFault(LockError::ResourceFailure); return false; }
    status.state = LockState::LockedOutput;
    status.error = LockError::None;
    publish();
    if (xTaskCreate(worker, "LockOutput", 3072, nullptr, 3, &taskHandle) != pdPASS) {
        latchFault(LockError::ResourceFailure);
        return false;
    }
    return true;
}

bool triggerLock(int durationMS) {
    using namespace lock_detail;
    if (!HardwareConfig::LOCK_ENABLED || !mutex || !taskHandle || durationMS <= 0 ||
        static_cast<uint32_t>(durationMS) > HardwareConfig::LOCK_MAX_PULSE_MS) return false;
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return false;
    const bool available = status.state == LockState::LockedOutput &&
                           cancelEpoch.load() == handledCancelEpoch;
    if (available) {
        if (++status.pulseNumber == 0) ++status.pulseNumber;
        status.pulseStarted = false;
        status.state = LockState::PendingUnlock;
        status.error = LockError::None;
        pendingDurationMs = static_cast<uint32_t>(durationMS);
        acceptedUs = esp_timer_get_time();
        publish();
    }
    xSemaphoreGive(mutex);
    return available;
}

bool lockNow() {
    using namespace lock_detail;
    if (!HardwareConfig::LOCK_ENABLED) return true;
    const uint32_t epoch = cancelEpoch.fetch_add(1) + 1;
    if (!mutex || !gpioConfigured) return false;
    if (xSemaphoreTake(mutex, ticksAtLeastOne(10)) != pdTRUE) return false;
    const bool ok = cancelLocked(epoch);
    xSemaphoreGive(mutex);
    return ok;
}

LockStatus getLockStatus() {
    LockStatus snapshot{};
    if (lock_detail::statusQueue)
        xQueuePeek(lock_detail::statusQueue, &snapshot, 0);
    return snapshot;
}
