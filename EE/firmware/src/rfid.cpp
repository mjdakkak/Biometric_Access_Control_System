#include "rfid.h"
#include "HardwareConfig.h"
#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Implementation state is private to the RFID worker, except queue handles
// initialized in setup. Other tasks interact only through the public functions.
namespace rfid_detail {
struct ScanCommand {
    RfidScanMode mode = RfidScanMode::Disabled;
    uint32_t generation = 0;
};

QueueHandle_t commandQueue = nullptr; // Length 1: latest requested mode wins.
QueueHandle_t eventQueue = nullptr;
QueueHandle_t statusQueue = nullptr;  // Length 1: latest status snapshot.
TaskHandle_t taskHandle = nullptr;
MFRC522 reader(HardwareConfig::RFID_SS_PIN, HardwareConfig::RFID_RST_PIN);

// Only the RFID task accesses the following state and the reader/SPI object.
ScanCommand command{};
RfidStatus status{};
bool spiStarted = false;
bool attemptedInit = false;
uint32_t lastInitMs = 0;
bool presentationAllowed = false;
bool trackingEmpty = false;
uint32_t emptySinceMs = 0;
bool completedForCommand = false;
bool hasPendingEvent = false;
RfidEvent pendingEvent{};
uint8_t consecutiveReadErrors = 0;

void publishStatus(RfidReaderState state, uint8_t version = 0) {
    status.state = state;
    status.version = version;
    xQueueOverwrite(statusQueue, &status);
}

void requireFreshPresentation() {
    presentationAllowed = false;
    trackingEmpty = false;
    consecutiveReadErrors = 0;
}

void applyNewestCommand() {
    ScanCommand next{};
    if (xQueueReceive(commandQueue, &next, 0) != pdTRUE) return;
    if (next.mode == command.mode && next.generation == command.generation) return;
    command = next;
    hasPendingEvent = false;
    pendingEvent = RfidEvent{};
    completedForCommand = false;
    // A card already left on the reader must not become a new user's scan.
    // Every new scanning window first requires an observed empty interval.
    requireFreshPresentation();
}

bool knownReaderVersion(uint8_t version) {
    // Recognized by the MFRC522 1.4.11 diagnostics, including common clones.
    // A recognized register value is just a communication sanity check.
    return version == 0x90 || version == 0x91 || version == 0x92 ||
           version == 0x88 || version == 0x12;
}

bool formatUid(const uint8_t *bytes, uint8_t count, char *out, size_t capacity) {
    if (out == nullptr || capacity == 0) return false;
    out[0] = '\0';
    if (bytes == nullptr || (count != 4 && count != 7 && count != 10) ||
        capacity < static_cast<size_t>(count) * 2 + 1) return false;
    const char hex[] = "0123456789ABCDEF";
    for (uint8_t i = 0; i < count; ++i) {
        out[2 * i] = hex[bytes[i] >> 4];
        out[2 * i + 1] = hex[bytes[i] & 0x0F];
    }
    out[2 * count] = '\0';
    return true;
}

bool ensureReaderReady() {
    if (!HardwareConfig::RFID_ENABLED) return false;
    if (!spiStarted) {
        // Global SPI is the SPI instance used internally by MFRC522 1.4.x.
        SPI.begin(HardwareConfig::RFID_SCK_PIN, HardwareConfig::RFID_MISO_PIN,
                  HardwareConfig::RFID_MOSI_PIN, HardwareConfig::RFID_SS_PIN);
        spiStarted = true;
    }
    if (status.state == RfidReaderState::Ready) {
        const uint8_t version = reader.PCD_ReadRegister(MFRC522::VersionReg);
        if (version == status.version && knownReaderVersion(version)) return true;
        publishStatus(RfidReaderState::Unavailable, version);
        requireFreshPresentation();
        hasPendingEvent = false;
        completedForCommand = false;
        lastInitMs = millis();
        attemptedInit = true;
        return false;
    }
    if (attemptedInit && millis() - lastInitMs < HardwareConfig::RFID_RETRY_MS)
        return false;
    attemptedInit = true;
    lastInitMs = millis();
    // Library init/reset is bounded but can take tens/hundreds of ms. It is
    // deliberately inside THIS task, never the Nextion/controller task.
    reader.PCD_Init();
    vTaskDelay(pdMS_TO_TICKS(5));
    const uint8_t first = reader.PCD_ReadRegister(MFRC522::VersionReg);
    const uint8_t second = reader.PCD_ReadRegister(MFRC522::VersionReg);
    const bool ready = first == second && knownReaderVersion(first);
    publishStatus(ready ? RfidReaderState::Ready : RfidReaderState::Unavailable,
                  second);
    requireFreshPresentation();
    return ready;
}

void sendOrKeepPending() {
    if (hasPendingEvent && xQueueSend(eventQueue, &pendingEvent, 0) == pdTRUE) {
        hasPendingEvent = false;
        pendingEvent = RfidEvent{};
    }
}

void finishCommand(RfidEventType type, const char *uid = nullptr,
                   uint8_t length = 0) {
    pendingEvent = RfidEvent{};
    pendingEvent.type = type;
    pendingEvent.mode = command.mode;
    pendingEvent.generation = command.generation;
    pendingEvent.uidLength = length;
    if (uid != nullptr) memcpy(pendingEvent.uid, uid, static_cast<size_t>(length) * 2 + 1);
    hasPendingEvent = true;
    completedForCommand = true; // At most one event per scanning operation.
    sendOrKeepPending();
}

void recoverRadioAfterError() {
    reader.PCD_StopCrypto1();
    reader.PCD_AntennaOff();
    vTaskDelay(pdMS_TO_TICKS(10));
    reader.PCD_AntennaOn();
    vTaskDelay(pdMS_TO_TICKS(5));
}

void tickWorker() {
    applyNewestCommand();
    if (!HardwareConfig::RFID_ENABLED) return; // No SPI/GPIO activity in code-only mode.
    if (!ensureReaderReady()) return;
    applyNewestCommand(); // A controller cancel may have arrived during init.
    if (command.mode == RfidScanMode::Disabled || command.generation == 0) return;
    sendOrKeepPending();
    if (completedForCommand) return;

    const ScanCommand startedCommand = command;
    uint8_t atqa[2] = {};
    uint8_t atqaLength = sizeof(atqa);
    // WUPA also checks halted cards. REQA/IsNewCardPresent alone cannot reliably
    // distinguish a halted, still-held card from one removed from the antenna.
    const MFRC522::StatusCode response = reader.PICC_WakeupA(atqa, &atqaLength);
    char uid[RFID_UID_TEXT_CAPACITY] = {};
    uint8_t uidLength = 0;
    bool readSucceeded = false;
    if (response == MFRC522::STATUS_OK) {
        if (reader.PICC_ReadCardSerial()) {
            uidLength = reader.uid.size;
            readSucceeded = formatUid(reader.uid.uidByte, uidLength, uid, sizeof(uid));
            const MFRC522::StatusCode halted = reader.PICC_HaltA();
            if (halted != MFRC522::STATUS_OK) readSucceeded = false;
        }
        reader.PCD_StopCrypto1();
    }
    if (response != MFRC522::STATUS_TIMEOUT && !readSucceeded)
        recoverRadioAfterError();

    // Commands can change while SPI work is in progress. Never stamp an old
    // physical read with a newer generation. The UI checks the token again.
    applyNewestCommand();
    if (command.mode != startedCommand.mode ||
        command.generation != startedCommand.generation) return;

    if (response == MFRC522::STATUS_TIMEOUT) {
        consecutiveReadErrors = 0;
        if (!trackingEmpty) {
            trackingEmpty = true;
            emptySinceMs = millis();
        }
        if (millis() - emptySinceMs >= HardwareConfig::RFID_REMOVAL_MS)
            presentationAllowed = true;
        return;
    }
    trackingEmpty = false; // Any non-timeout response breaks the empty interval.
    if (!readSucceeded) {
        // A collision or malformed response is NOT proof the reader is empty.
        presentationAllowed = false;
        if (++consecutiveReadErrors >= 3) finishCommand(RfidEventType::ReadError);
        return;
    }
    consecutiveReadErrors = 0;
    if (!presentationAllowed) return; // Remove first, then present one card.
    finishCommand(RfidEventType::CardScanned, uid, uidLength);
}

void workerTask(void *) {
    while (true) {
        tickWorker();
        vTaskDelay(pdMS_TO_TICKS(HardwareConfig::RFID_POLL_MS));
    }
}
} // namespace rfid_detail

bool beginRFID() {
    using namespace rfid_detail;
    if (commandQueue != nullptr || eventQueue != nullptr || statusQueue != nullptr)
        return false;
    commandQueue = xQueueCreate(1, sizeof(ScanCommand));
    eventQueue = xQueueCreate(4, sizeof(RfidEvent));
    statusQueue = xQueueCreate(1, sizeof(RfidStatus));
    if (commandQueue && eventQueue && statusQueue) {
        publishStatus(HardwareConfig::RFID_ENABLED ? RfidReaderState::Starting
                                                  : RfidReaderState::Disabled);
        if (xTaskCreate(workerTask, "RFID", 4096, nullptr, 1, &taskHandle) == pdPASS)
            return true;
    }
    if (commandQueue) vQueueDelete(commandQueue);
    if (eventQueue) vQueueDelete(eventQueue);
    if (statusQueue) vQueueDelete(statusQueue);
    commandQueue = nullptr;
    eventQueue = nullptr;
    statusQueue = nullptr;
    taskHandle = nullptr;
    return false;
}

bool setRFIDScanMode(RfidScanMode mode, uint32_t generation) {
    if (rfid_detail::commandQueue == nullptr || generation == 0 ||
        (mode != RfidScanMode::Disabled && mode != RfidScanMode::Authentication &&
         mode != RfidScanMode::Enrollment)) return false;
    rfid_detail::ScanCommand command{};
    command.mode = mode;
    command.generation = generation;
    return xQueueOverwrite(rfid_detail::commandQueue, &command) == pdTRUE;
}

bool pollRFIDEvent(RfidEvent &event) {
    return rfid_detail::eventQueue != nullptr &&
           xQueueReceive(rfid_detail::eventQueue, &event, 0) == pdTRUE;
}

RfidStatus getRFIDStatus() {
    RfidStatus status{};
    if (rfid_detail::statusQueue != nullptr)
        xQueuePeek(rfid_detail::statusQueue, &status, 0);
    return status;
}
