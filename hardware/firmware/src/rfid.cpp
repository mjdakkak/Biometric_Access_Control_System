#include "rfid.h"
#include "HardwareConfig.h"
#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// The RFID worker owns the reader and SPI bus.
namespace rfid_detail {
struct ScanCommand {
    RfidScanMode mode = RfidScanMode::Disabled;
    uint32_t generation = 0;
};

QueueHandle_t commandQueue = nullptr; // Latest mode wins.
QueueHandle_t eventQueue = nullptr;
QueueHandle_t statusQueue = nullptr;
TaskHandle_t taskHandle = nullptr;
MFRC522 reader(HardwareConfig::RFID_SS_PIN, HardwareConfig::RFID_RST_PIN);

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
    // Require card removal before each new scan window.
    requireFreshPresentation();
}

bool knownReaderVersion(uint8_t version) {
    // A known version value checks communication, not RF performance.
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
    completedForCommand = true; // One scan result per operation.
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
    if (!HardwareConfig::RFID_ENABLED) return;
    if (!ensureReaderReady()) return;
    applyNewestCommand();
    if (command.mode == RfidScanMode::Disabled || command.generation == 0) return;
    sendOrKeepPending();
    if (completedForCommand) return;

    const ScanCommand startedCommand = command;
    uint8_t atqa[2] = {};
    uint8_t atqaLength = sizeof(atqa);
    // WUPA detects held, halted cards that REQA may miss.
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

    // Keep the original generation if a command changes during the SPI read.
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
    trackingEmpty = false;
    if (!readSucceeded) {
        // A collision is not proof that the reader is empty.
        presentationAllowed = false;
        if (++consecutiveReadErrors >= 3) finishCommand(RfidEventType::ReadError);
        return;
    }
    consecutiveReadErrors = 0;
    if (!presentationAllowed) return;
    finishCommand(RfidEventType::CardScanned, uid, uidLength);
}

void workerTask(void *) {
    while (true) {
        tickWorker();
        vTaskDelay(pdMS_TO_TICKS(HardwareConfig::RFID_POLL_MS));
    }
}
}

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
