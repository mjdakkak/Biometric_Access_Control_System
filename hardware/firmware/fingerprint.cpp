#include "fingerprint.h"
#include "fingerprint_recovery.h"
#include <atomic>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include "HardwareConfig.h"
#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// This worker owns UART1 and the sensor-operation journal.
// Reuse the packet writer; validate replies with bounded reads and deadlines.
namespace fingerprint_detail {
using namespace HardwareConfig;
constexpr uint8_t READ_INDEX_TABLE = 0x1F;
constexpr uint32_t JOURNAL_MAGIC = 0x46504A31; // FPJ1

struct Ack {
    uint8_t payload[33] = {};
    uint8_t size = 0;
};
class As608Device {
public:
    HardwareSerial uart;
    Adafruit_Fingerprint packetWriter;
    uint16_t capacity = 0;
    explicit As608Device() : uart(1), packetWriter(&uart, FINGER_PASSWORD) {}

    // One command in flight. Ignore fields in error replies.
    uint8_t transact(uint8_t *data, uint8_t size, uint8_t successSize, Ack &ack) {
        ack = Ack{};
        unsigned drained = 0;
        while (uart.available() && drained < 256) { uart.read(); ++drained; }
        if (uart.available()) return FINGERPRINT_BADPACKET;
        Adafruit_Fingerprint_Packet packet(FINGERPRINT_COMMANDPACKET, size, data);
        for (unsigned i = 0; i < 4; ++i)
            packet.address[i] = static_cast<uint8_t>(FINGER_ADDRESS >> (24 - 8 * i));
        packetWriter.writeStructuredPacket(packet);

        uint8_t wire[44] = {}; // Header + largest reply + checksum.
        size_t used = 0, expected = 0;
        const uint32_t started = millis();
        unsigned work = 0;
        while (millis() - started < FINGER_COMMAND_TIMEOUT_MS) {
            if (!uart.available()) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
            const int value = uart.read();
            if (value < 0) continue;
            const uint8_t b = static_cast<uint8_t>(value);
            if (++work % 64 == 0) vTaskDelay(pdMS_TO_TICKS(1));
            if (used == 0 && b != 0xEF) continue;
            if (used >= sizeof(wire)) return FINGERPRINT_BADPACKET;
            wire[used++] = b;
            if (used == 2 && wire[1] != 0x01) return FINGERPRINT_BADPACKET;
            if (used == 9) {
                const uint16_t length = (static_cast<uint16_t>(wire[7]) << 8) | wire[8];
                if (wire[6] != FINGERPRINT_ACKPACKET || length < 3 || length > 35)
                    return FINGERPRINT_BADPACKET;
                for (unsigned i = 0; i < 4; ++i)
                    if (wire[2+i] != static_cast<uint8_t>(FINGER_ADDRESS >> (24-8*i)))
                        return FINGERPRINT_BADPACKET;
                expected = 9 + length;
            }
            if (expected && used == expected) {
                uint16_t sum = 0;
                for (size_t i = 6; i < expected - 2; ++i) sum += wire[i];
                const uint16_t received = (static_cast<uint16_t>(wire[expected-2]) << 8)
                                         | wire[expected-1];
                if (sum != received) return FINGERPRINT_BADPACKET;
                ack.size = static_cast<uint8_t>(expected - 11);
                memcpy(ack.payload, wire + 9, ack.size);
                if (ack.payload[0] == FINGERPRINT_OK && ack.size != successSize)
                    return FINGERPRINT_BADPACKET;
                if (ack.payload[0] != FINGERPRINT_OK && ack.size != 1 &&
                    ack.size != successSize) return FINGERPRINT_BADPACKET;
                return ack.payload[0];
            }
        }
        return FINGERPRINT_TIMEOUT;
    }
    uint8_t verifyPassword() {
        uint8_t d[] = {FINGERPRINT_VERIFYPASSWORD,
            static_cast<uint8_t>(FINGER_PASSWORD >> 24),
            static_cast<uint8_t>(FINGER_PASSWORD >> 16),
            static_cast<uint8_t>(FINGER_PASSWORD >> 8),
            static_cast<uint8_t>(FINGER_PASSWORD)};
        Ack a; return transact(d, sizeof(d), 1, a);
    }
    uint8_t getParameters() {
        uint8_t d[] = {FINGERPRINT_READSYSPARAM}; Ack a;
        const uint8_t code = transact(d, sizeof(d), 17, a);
        if (code == FINGERPRINT_OK)
            capacity = (static_cast<uint16_t>(a.payload[5]) << 8) | a.payload[6];
        return code;
    }
    uint8_t getImage() { uint8_t d[] = {FINGERPRINT_GETIMAGE}; Ack a; return transact(d,1,1,a); }
    uint8_t image2Tz(uint8_t buffer) {
        uint8_t d[] = {FINGERPRINT_IMAGE2TZ, buffer}; Ack a; return transact(d,2,1,a);
    }
    uint8_t createModel() { uint8_t d[] = {FINGERPRINT_REGMODEL}; Ack a; return transact(d,1,1,a); }
    uint8_t storeModel(uint16_t slot) {
        uint8_t d[] = {FINGERPRINT_STORE,1,static_cast<uint8_t>(slot>>8),static_cast<uint8_t>(slot)};
        Ack a; return transact(d,sizeof(d),1,a);
    }
    uint8_t loadModel(uint16_t slot) {
        uint8_t d[] = {FINGERPRINT_LOAD,1,static_cast<uint8_t>(slot>>8),static_cast<uint8_t>(slot)};
        Ack a; return transact(d,sizeof(d),1,a);
    }
    uint8_t deleteModel(uint16_t slot) {
        uint8_t d[] = {FINGERPRINT_DELETE,static_cast<uint8_t>(slot>>8),static_cast<uint8_t>(slot),0,1};
        Ack a; return transact(d,sizeof(d),1,a);
    }
    uint8_t search(uint16_t start, uint16_t count, uint16_t &slot, uint16_t &confidence) {
        slot = 0xFFFF; confidence = 0;
        uint8_t d[] = {FINGERPRINT_SEARCH,1,static_cast<uint8_t>(start>>8),static_cast<uint8_t>(start),
            static_cast<uint8_t>(count>>8),static_cast<uint8_t>(count)};
        Ack a; const uint8_t code = transact(d,sizeof(d),5,a);
        if (code == FINGERPRINT_OK) {
            slot = (static_cast<uint16_t>(a.payload[1])<<8) | a.payload[2];
            confidence = (static_cast<uint16_t>(a.payload[3])<<8) | a.payload[4];
            if (slot < start || static_cast<uint32_t>(slot) >= static_cast<uint32_t>(start)+count)
                return FINGERPRINT_BADPACKET;
        }
        return code;
    }
    uint8_t slotUsed(uint16_t slot, bool &used) {
        used = false;
        uint8_t d[] = {READ_INDEX_TABLE,static_cast<uint8_t>(slot/256)};
        Ack a; const uint8_t code = transact(d,sizeof(d),33,a);
        if (code == FINGERPRINT_OK) used = (a.payload[1+(slot%256)/8] & (1U << (slot%8))) != 0;
        return code;
    }
};

enum class Phase : uint8_t {
    Start, CheckProtected, CheckTarget, ClearFirst, CaptureFirst, ConvertFirst,
    CheckDifferent, RemoveBetween, CaptureSecond, ConvertSecond, BuildModel,
    RecheckTarget, JournalStore, Store, VerifyStoreIndex, VerifyStoreLoad,
    JournalDelete, DeleteOne, VerifyDelete, Search, Done
};
struct MutationAck {
    uint32_t generation = 0;
    FingerprintOperation operation = FingerprintOperation::None;
    char sessionId[FP_SESSION_CAPACITY] = "";
};
struct JournalRecord {
    uint32_t magic;
    FingerprintCommand job;
    uint32_t checksum;
};
static_assert(std::is_trivially_copyable<JournalRecord>::value, "NVS record must be plain data");
QueueHandle_t commandQueue = nullptr, eventQueue = nullptr, statusQueue = nullptr, ackQueue = nullptr;
TaskHandle_t taskHandle = nullptr;
As608Device device;
Preferences prefs;
FingerprintStatus status{};
FingerprintCommand command{};
JournalRecord journal{};
Phase phase = Phase::Done;
bool preferencesOpened = false, preferencesAttempted = false;
bool journalActive = false, journalFromBoot = false, mutationComplete = false;
bool serialStarted = false, sensorReady = false, attemptedInit = false;
uint32_t serialStartedMs = 0, lastInitMs = 0, operationStartedMs = 0;
uint32_t emptySinceMs = 0;
bool trackingEmpty = false, pendingTerminal = false;
FingerprintEvent terminal{};
uint8_t cursor = 0, deletedCount = 0, badCaptures = 0;

bool cString(const char *s, size_t n) { return memchr(s, '\0', n) != nullptr; }
bool slotValid(int slot) { return slot >= 0 && static_cast<unsigned>(slot) < status.capacity; }
bool communicationError(uint8_t c) {
    return c == FINGERPRINT_TIMEOUT || c == FINGERPRINT_BADPACKET || c == FINGERPRINT_PACKETRECIEVEERR;
}
void publish(FingerprintReaderState state) {
    status.state = state; status.mutationPending = journalActive;
    xQueueOverwrite(statusQueue, &status);
}
uint32_t recordChecksum(const JournalRecord &r) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&r);
    uint32_t hash = 2166136261U;
    for (size_t i=0; i<offsetof(JournalRecord,checksum); ++i) hash=(hash^p[i])*16777619U;
    return hash; // Corruption check, not authentication.
}
bool openJournal() {
    if (preferencesAttempted) return preferencesOpened;
    preferencesAttempted = true;
    preferencesOpened = prefs.begin("kiosk_fp", false);
    if (!preferencesOpened) { publish(FingerprintReaderState::Unavailable); return false; }
    const size_t length = prefs.getBytesLength("pending");
    if (length) {
        memset(&journal, 0, sizeof(journal));
        if (length == sizeof(journal)) prefs.getBytes("pending", &journal, sizeof(journal));
        // A pending record survives reboot until the result is reconciled.
        journalActive = true; journalFromBoot = true;
        publish(FingerprintReaderState::RecoveryRequired);
    }
    return true;
}
bool saveIntent() {
    if (!preferencesOpened || journalActive) return false;
    memset(&journal, 0, sizeof(journal));
    journal.magic = JOURNAL_MAGIC; journal.job = command;
    journal.checksum = recordChecksum(journal);
    journalActive = true; mutationComplete = false;
    // Save the intent before changing sensor storage.
    if (prefs.putBytes("pending", &journal, sizeof(journal)) != sizeof(journal)) {
        publish(FingerprintReaderState::RecoveryRequired); return false;
    }
    JournalRecord check{};
    if (prefs.getBytes("pending", &check, sizeof(check)) != sizeof(check) ||
        memcmp(&check, &journal, sizeof(journal)) != 0) {
        publish(FingerprintReaderState::RecoveryRequired); return false;
    }
    publish(FingerprintReaderState::Ready);
    return true;
}
void processAcks() {
    MutationAck ack{};
    while (xQueueReceive(ackQueue, &ack, 0) == pdTRUE) {
        if (!journalActive || journalFromBoot || !mutationComplete ||
            journal.magic != JOURNAL_MAGIC || journal.checksum != recordChecksum(journal) ||
            ack.generation != journal.job.generation || ack.operation != journal.job.operation ||
            strcmp(ack.sessionId, journal.job.sessionId) != 0) continue;
        if (!prefs.remove("pending")) { publish(FingerprintReaderState::RecoveryRequired); continue; }
        journalActive = false; mutationComplete = false; memset(&journal,0,sizeof(journal));
        publish(sensorReady ? FingerprintReaderState::Ready : FingerprintReaderState::Unavailable);
    }
}
void emitProgress(FingerprintPrompt prompt) {
    FingerprintEvent e{}; e.type=FingerprintEventType::Progress;
    e.operation=command.operation; e.generation=command.generation; e.prompt=prompt;
    xQueueSend(eventQueue,&e,0);
}
void finish(FingerprintEventType type, FingerprintError error=FingerprintError::None,
            uint8_t code=0, int slot=-1, uint16_t confidence=0) {
    terminal=FingerprintEvent{}; terminal.type=type; terminal.operation=command.operation;
    terminal.generation=command.generation; terminal.error=error; terminal.sensorCode=code;
    terminal.slot=slot; terminal.confidence=confidence; terminal.deletedCount=deletedCount;
    pendingTerminal=true; phase=Phase::Done;
    if (communicationError(code)) { sensorReady=false; attemptedInit=true; lastInitMs=millis(); }
    if (type==FingerprintEventType::Failed && journalActive)
        publish(FingerprintReaderState::RecoveryRequired);
    else publish(sensorReady ? FingerprintReaderState::Ready : status.state);
}
void reject(FingerprintError error, uint8_t code=0) { finish(FingerprintEventType::Failed,error,code); }
void rejectSensor(uint8_t code, FingerprintError other=FingerprintError::SensorRejected) {
    reject(communicationError(code) ? FingerprintError::SensorCommunication : other,code);
}
void applyCommand() {
    FingerprintCommand next{};
    if (xQueueReceive(commandQueue,&next,0)!=pdTRUE) return;
    if (next.generation==command.generation && next.operation==command.operation) return;
    command=next; pendingTerminal=false; cursor=0; deletedCount=0; badCaptures=0;
    trackingEmpty=false; operationStartedMs=millis();
    phase=command.operation==FingerprintOperation::None ? Phase::Done : Phase::Start;
}
bool stillCurrent() {
    FingerprintCommand next{};
    return xQueuePeek(commandQueue,&next,0)!=pdTRUE ||
           (next.generation==command.generation && next.operation==command.operation);
}
bool ensureSensor(bool forRecovery=false) {
    if (!openJournal() || (journalFromBoot && !forRecovery)) return false;
    if (sensorReady) return true;
    if (!serialStarted) {
        device.uart.setRxBufferSize(256);
        device.uart.begin(FINGER_BAUD,SERIAL_8N1,FINGER_RX_PIN,FINGER_TX_PIN);
        serialStarted=static_cast<bool>(device.uart); serialStartedMs=millis();
        publish(serialStarted ? FingerprintReaderState::Starting : FingerprintReaderState::Unavailable);
        return false;
    }
    if (millis()-serialStartedMs<FINGER_BOOT_MS) return false;
    if (attemptedInit && millis()-lastInitMs<FINGER_RETRY_MS) return false;
    attemptedInit=true; lastInitMs=millis();
    uint8_t code=device.verifyPassword();
    if (code==FINGERPRINT_OK) code=device.getParameters();
    if (code!=FINGERPRINT_OK || device.capacity==0 || device.capacity>FINGER_MAX_SUPPORTED_CAPACITY) {
        publish(FingerprintReaderState::Unavailable); return false;
    }
    status.capacity=device.capacity; sensorReady=true; publish(journalFromBoot ? FingerprintReaderState::RecoveryRequired : FingerprintReaderState::Ready);
    return true;
}
bool validateCommand() {
    if (!command.generation || !cString(command.sessionId,sizeof(command.sessionId)) || !command.sessionId[0] ||
        command.protectedCount>FP_MAX_SLOTS || command.deleteCount>FP_MAX_SLOTS) return false;
    if (command.operation==FingerprintOperation::Enroll &&
        (!slotValid(command.targetSlot) || command.protectedCount>1)) return false;
    if (command.operation==FingerprintOperation::DeleteOld && command.protectedCount!=2) return false;
    for (unsigned i=0;i<command.protectedCount;++i) {
        if (!slotValid(command.protectedSlots[i]) ||
            (command.operation==FingerprintOperation::Enroll && command.targetSlot==command.protectedSlots[i])) return false;
        for (unsigned j=0;j<i;++j) if (command.protectedSlots[i]==command.protectedSlots[j]) return false;
    }
    for (unsigned i=0;i<command.deleteCount;++i) {
        if (!slotValid(command.deleteSlots[i])) return false;
        for (unsigned j=0;j<i;++j) if (command.deleteSlots[i]==command.deleteSlots[j]) return false;
        for (unsigned j=0;j<command.protectedCount;++j)
            if (command.deleteSlots[i]==command.protectedSlots[j]) return false;
    }
    return true;
}
void startClear() { trackingEmpty=false; phase=Phase::ClearFirst; emitProgress(FingerprintPrompt::RemoveFinger); }
void waitClear(Phase next, FingerprintPrompt prompt) {
    const uint8_t c=device.getImage();
    if (c==FINGERPRINT_NOFINGER) {
        if (!trackingEmpty) { trackingEmpty=true; emptySinceMs=millis(); }
        if (millis()-emptySinceMs>=FINGER_REMOVAL_MS) {
            phase=next; trackingEmpty=false; emitProgress(prompt);
        }
    } else if (c==FINGERPRINT_OK || c==FINGERPRINT_IMAGEFAIL) trackingEmpty=false;
    else rejectSensor(c);
}
void imageError(uint8_t c) {
    if (c==FINGERPRINT_IMAGEFAIL || c==FINGERPRINT_IMAGEMESS ||
        c==FINGERPRINT_FEATUREFAIL || c==FINGERPRINT_INVALIDIMAGE) {
        if (++badCaptures>=FINGER_MAX_BAD_CAPTURES) { reject(FingerprintError::BadImage,c); return; }
        // A poor sample restarts both captures within the original deadline.
        startClear(); emitProgress(FingerprintPrompt::PoorImage);
    } else rejectSensor(c);
}
uint32_t deadlineMs() {
    if (command.operation==FingerprintOperation::Match) return FINGER_MATCH_TIMEOUT_MS;
    if (command.operation==FingerprintOperation::DeleteOld) return FINGER_DELETE_TIMEOUT_MS;
    return FINGER_ENROLL_TIMEOUT_MS;
}
void stepOperation() {
    bool used=false; uint8_t c=FINGERPRINT_OK;
    switch (phase) {
    case Phase::Start:
        if (journalActive) { reject(FingerprintError::RecoveryRequired); break; }
        if (!validateCommand()) { reject(FingerprintError::InvalidCommand); break; }
        if (command.operation==FingerprintOperation::Match) startClear();
        else { phase=Phase::CheckProtected; cursor=0; emitProgress(FingerprintPrompt::Processing); }
        break;
    case Phase::CheckProtected:
        if (cursor<command.protectedCount) {
            c=device.slotUsed(command.protectedSlots[cursor],used);
            if (c!=FINGERPRINT_OK) reject(FingerprintError::IndexUnavailable,c);
            else if (!used) reject(FingerprintError::InvalidSlot);
            else ++cursor;
        } else {
            cursor=0;
            phase=command.operation==FingerprintOperation::Enroll ? Phase::CheckTarget : Phase::JournalDelete;
        }
        break;
    case Phase::CheckTarget:
    case Phase::RecheckTarget:
        c=device.slotUsed(command.targetSlot,used);
        if (c!=FINGERPRINT_OK) reject(FingerprintError::IndexUnavailable,c);
        else if (used) reject(FingerprintError::SlotOccupied);
        else if (phase==Phase::CheckTarget) startClear();
        else phase=Phase::JournalStore;
        break;
    case Phase::ClearFirst: waitClear(Phase::CaptureFirst,FingerprintPrompt::PlaceFinger); break;
    case Phase::CaptureFirst:
    case Phase::CaptureSecond:
        c=device.getImage();
        if (c==FINGERPRINT_OK) {
            phase=phase==Phase::CaptureFirst ? Phase::ConvertFirst : Phase::ConvertSecond;
            emitProgress(FingerprintPrompt::Processing);
        } else if (c!=FINGERPRINT_NOFINGER) imageError(c);
        break;
    case Phase::ConvertFirst:
        c=device.image2Tz(1);
        if (c!=FINGERPRINT_OK) imageError(c);
        else if (command.operation==FingerprintOperation::Match) phase=Phase::Search;
        else { phase=Phase::CheckDifferent; cursor=0; }
        break;
    case Phase::CheckDifferent:
        if (cursor<command.protectedCount) {
            uint16_t slot=0xFFFF,confidence=0;
            c=device.search(command.protectedSlots[cursor],1,slot,confidence);
            if (c==FINGERPRINT_OK) {
                if (++badCaptures>=FINGER_MAX_BAD_CAPTURES) reject(FingerprintError::DuplicateFinger);
                else { startClear(); emitProgress(FingerprintPrompt::UseDifferentFinger); }
            } else if (c==FINGERPRINT_NOTFOUND) ++cursor;
            else rejectSensor(c);
        } else {
            phase=Phase::RemoveBetween; trackingEmpty=false; emitProgress(FingerprintPrompt::RemoveFinger);
        }
        break;
    case Phase::RemoveBetween: waitClear(Phase::CaptureSecond,FingerprintPrompt::PlaceSameFinger); break;
    case Phase::ConvertSecond:
        c=device.image2Tz(2);
        if (c!=FINGERPRINT_OK) imageError(c); else phase=Phase::BuildModel;
        break;
    case Phase::BuildModel:
        c=device.createModel();
        if (c==FINGERPRINT_ENROLLMISMATCH) reject(FingerprintError::SamplesDiffer,c);
        else if (c!=FINGERPRINT_OK) rejectSensor(c);
        else phase=Phase::RecheckTarget;
        break;
    case Phase::JournalStore:
        if (!stillCurrent()) break;
        if (!saveIntent()) reject(FingerprintError::JournalFailed);
        else { phase=Phase::Store; emitProgress(FingerprintPrompt::Storing); }
        break;
    case Phase::Store:
        // A sent write cannot be cancelled; keep its journal until the server confirms it.
        if (!stillCurrent()) break;
        c=device.storeModel(command.targetSlot);
        if (c!=FINGERPRINT_OK) reject(FingerprintError::StorageFailed,c);
        else phase=Phase::VerifyStoreIndex;
        break;
    case Phase::VerifyStoreIndex:
        c=device.slotUsed(command.targetSlot,used);
        if (c!=FINGERPRINT_OK || !used) reject(FingerprintError::StorageFailed,c);
        else phase=Phase::VerifyStoreLoad;
        break;
    case Phase::VerifyStoreLoad:
        c=device.loadModel(command.targetSlot);
        if (c!=FINGERPRINT_OK) reject(FingerprintError::StorageFailed,c);
        else { mutationComplete=true; finish(FingerprintEventType::Enrolled,FingerprintError::None,0,command.targetSlot); }
        break;
    case Phase::JournalDelete:
        if (command.deleteCount==0) { finish(FingerprintEventType::OldDeleted); break; }
        if (!stillCurrent()) break;
        if (!saveIntent()) reject(FingerprintError::JournalFailed);
        else { cursor=0; phase=Phase::DeleteOne; emitProgress(FingerprintPrompt::Deleting); }
        break;
    case Phase::DeleteOne:
        if (!stillCurrent()) break;
        c=device.deleteModel(command.deleteSlots[cursor]);
        if (c!=FINGERPRINT_OK) reject(FingerprintError::DeleteFailed,c);
        else phase=Phase::VerifyDelete;
        break;
    case Phase::VerifyDelete:
        c=device.slotUsed(command.deleteSlots[cursor],used);
        if (c!=FINGERPRINT_OK || used) { reject(FingerprintError::DeleteFailed,c); break; }
        ++deletedCount; ++cursor;
        if (cursor<command.deleteCount) phase=Phase::DeleteOne;
        else { mutationComplete=true; finish(FingerprintEventType::OldDeleted); }
        break;
    case Phase::Search: {
        uint16_t slot=0xFFFF,confidence=0;
        c=device.search(0,status.capacity,slot,confidence);
        if (c==FINGERPRINT_OK) finish(FingerprintEventType::Matched,FingerprintError::None,0,slot,confidence);
        else if (c==FINGERPRINT_NOTFOUND) finish(FingerprintEventType::NoMatch);
        else rejectSensor(c);
        break;
    }
    case Phase::Done: break;
    }
}
// Recovery runs on the same worker as normal sensor commands.
enum class RecoveryMessageKind : uint8_t { Inspect, Apply, Acknowledge };
struct RecoveryMessage {
    RecoveryMessageKind kind=RecoveryMessageKind::Inspect;
    uint32_t requestId=0;
    FingerprintRecoveryPlan plan{};
    FingerprintRecoveryAcknowledgement acknowledgement{};
};
QueueHandle_t recoveryQueue=nullptr,recoveryReportQueue=nullptr,recoveryReceiptQueue=nullptr;
std::atomic<bool> recoveryCancelled{false};
FingerprintRecoveryReport recoveryReport{};
FingerprintRecoveryReceipt recoveryReceipt{};
JournalRecord recoveryRecord{};
bool recoveryHasReport=false,recoveryAwaitingAck=false;
uint32_t recoveryReportedMs=0,recoveryStartedMs=0;

bool recoveryStopped(){return recoveryCancelled.load() || millis()-recoveryStartedMs>=FP_RECOVERY_IO_TIMEOUT_MS;}
FingerprintRecoveryResult stoppedResult(){
    return recoveryCancelled.load()?FingerprintRecoveryResult::Cancelled:FingerprintRecoveryResult::TimedOut;
}
void makeTicket(char (&out)[33]){
    uint8_t bytes[16];esp_fill_random(bytes,sizeof(bytes));
    const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<16;++i){out[2*i]=hex[bytes[i]>>4];out[2*i+1]=hex[bytes[i]&15];}
    out[32]=0;
}
bool digestRecord(const JournalRecord &r,char (&out)[65]){
    uint8_t bytes[32];
    if(mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>(&r),sizeof(r),bytes,0)!=0)return false;
    const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){out[2*i]=hex[bytes[i]>>4];out[2*i+1]=hex[bytes[i]&15];}
    out[64]=0;return true;
}
bool validJournalStructure(const JournalRecord &r){
    const auto &j=r.job;
    if(r.magic!=JOURNAL_MAGIC || r.checksum!=recordChecksum(r) || !j.generation ||
       !cString(j.sessionId,sizeof(j.sessionId)) || !j.sessionId[0] ||
       j.deleteCount>FP_MAX_SLOTS || j.protectedCount>FP_MAX_SLOTS)return false;
    if(j.operation==FingerprintOperation::Enroll){
        if(j.targetSlot<0||j.targetSlot>=FINGER_MAX_SUPPORTED_CAPACITY||j.protectedCount>1||j.deleteCount)return false;
    }else if(j.operation==FingerprintOperation::DeleteOld){
        if(j.protectedCount!=2||!j.deleteCount)return false;
    }else return false;
    for(unsigned i=0;i<j.protectedCount;++i){
        if(j.protectedSlots[i]<0||j.protectedSlots[i]>=FINGER_MAX_SUPPORTED_CAPACITY)return false;
        if(j.operation==FingerprintOperation::Enroll&&j.protectedSlots[i]==j.targetSlot)return false;
        for(unsigned k=0;k<i;++k)if(j.protectedSlots[i]==j.protectedSlots[k])return false;
    }
    for(unsigned i=0;i<j.deleteCount;++i){
        if(j.deleteSlots[i]<0||j.deleteSlots[i]>=FINGER_MAX_SUPPORTED_CAPACITY)return false;
        for(unsigned k=0;k<i;++k)if(j.deleteSlots[i]==j.deleteSlots[k])return false;
        for(unsigned k=0;k<j.protectedCount;++k)if(j.deleteSlots[i]==j.protectedSlots[k])return false;
    }
    return true;
}
bool readJournalExactly(JournalRecord &record){
    memset(&record,0,sizeof(record));
    return preferencesOpened && prefs.getBytesLength("pending")==sizeof(record) &&
        prefs.getBytes("pending",&record,sizeof(record))==sizeof(record) && validJournalStructure(record);
}
bool sameRecoveryJournal(){
    JournalRecord record{};
    return readJournalExactly(record)&&memcmp(&record,&recoveryRecord,sizeof(record))==0;
}
bool prepareRecoverySensor(){
    while(!recoveryStopped()){
        if(ensureSensor(true))return true;
        if(!preferencesOpened)return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}
bool observeSlot(int slot,FingerprintSlotObservation &observation){
    observation=FingerprintSlotObservation::Unknown;
    if(recoveryStopped()||!slotValid(slot))return false;
    bool used=false;uint8_t code=device.slotUsed(slot,used);
    if(code!=FINGERPRINT_OK){if(communicationError(code))sensorReady=false;return false;}
    if(!used){observation=FingerprintSlotObservation::Absent;return true;}
    if(recoveryStopped())return false;
    code=device.loadModel(slot); // Readability does not establish template ownership.
    if(communicationError(code)){sensorReady=false;return false;}
    observation=code==FINGERPRINT_OK?FingerprintSlotObservation::PresentReadable:FingerprintSlotObservation::PresentUnreadable;
    return true;
}
bool observeJournal(FingerprintRecoveryReport &report){
    const auto &j=recoveryRecord.job;
    report.sensorCapacity=status.capacity;
    if(j.operation==FingerprintOperation::Enroll&&!observeSlot(j.targetSlot,report.target))return false;
    for(unsigned i=0;i<j.protectedCount;++i)if(!observeSlot(j.protectedSlots[i],report.protectedSlots[i]))return false;
    for(unsigned i=0;i<j.deleteCount;++i)if(!observeSlot(j.deleteSlots[i],report.oldSlots[i]))return false;
    return !recoveryStopped();
}
bool protectedReadable(const FingerprintRecoveryReport &r){
    for(unsigned i=0;i<recoveryRecord.job.protectedCount;++i)
        if(r.protectedSlots[i]!=FingerprintSlotObservation::PresentReadable)return false;
    return true;
}
bool sameObservations(const FingerprintRecoveryReport &a,const FingerprintRecoveryReport &b){
    if(a.sensorCapacity!=b.sensorCapacity || a.target!=b.target)return false;
    for(unsigned i=0;i<FP_MAX_SLOTS;++i)
        if(a.oldSlots[i]!=b.oldSlots[i]||a.protectedSlots[i]!=b.protectedSlots[i])return false;
    return true;
}
void postRecoveryReceipt(FingerprintRecoveryResult result,uint32_t requestId,
                         FingerprintRecoveryAction action=FingerprintRecoveryAction::Hold,uint8_t removed=0){
    FingerprintRecoveryReceipt r{};r.requestId=requestId;r.result=result;r.action=action;
    memcpy(r.journalId,recoveryReport.journalId,sizeof(r.journalId));
    if(validJournalStructure(recoveryRecord))memcpy(r.sessionId,recoveryRecord.job.sessionId,sizeof(r.sessionId));
    r.removedThisAttempt=removed;
    if(result==FingerprintRecoveryResult::AppliedAwaitingBackend){
        makeTicket(r.ticket);recoveryReceipt=r;recoveryAwaitingAck=true;
    }
    // One receipt at a time; losing it must not clear the journal.
    xQueueOverwrite(recoveryReceiptQueue,&r);
}
void inspectRecovery(uint32_t requestId){
    FingerprintRecoveryReport r{};r.requestId=requestId;
    recoveryHasReport=false;recoveryAwaitingAck=false;
    if(!FINGERPRINT_ENABLED)r.result=FingerprintRecoveryResult::Disabled;
    else if(phase!=Phase::Done || command.operation!=FingerprintOperation::None)r.result=FingerprintRecoveryResult::Busy;
    else if(!openJournal())r.result=FingerprintRecoveryResult::StorageFailed;
    else if(!journalActive && !prefs.getBytesLength("pending"))r.result=FingerprintRecoveryResult::NoPending;
    else if(!readJournalExactly(recoveryRecord)){
        // Preserve invalid records for recovery instead of silently resetting them.
        journalActive=true;journalFromBoot=true;r.result=FingerprintRecoveryResult::InvalidJournal;
    }else{
        journalActive=true;journalFromBoot=true;
        journal=recoveryRecord;
        r.pending=recoveryRecord.job;
        if(!digestRecord(recoveryRecord,r.journalId))r.result=FingerprintRecoveryResult::StorageFailed;
        else if(!prepareRecoverySensor())r.result=recoveryStopped()?stoppedResult():FingerprintRecoveryResult::SensorUnavailable;
        else if(!observeJournal(r))r.result=recoveryStopped()?stoppedResult():FingerprintRecoveryResult::SensorUnavailable;
        else{
            makeTicket(r.ticket);r.result=FingerprintRecoveryResult::Inspected;
            recoveryReportedMs=millis();recoveryHasReport=true;
        }
    }
    recoveryReport=r;
    if(journalActive)publish(FingerprintReaderState::RecoveryRequired);
    xQueueOverwrite(recoveryReportQueue,&r);
}
bool desiredPostState(FingerprintRecoveryAction action,const FingerprintRecoveryReport &r){
    if(!protectedReadable(r))return false;
    if(action==FingerprintRecoveryAction::KeepStoredTemplate)
        return r.target==FingerprintSlotObservation::PresentReadable;
    if(action==FingerprintRecoveryAction::DiscardUncommittedTemplate)
        return r.target==FingerprintSlotObservation::Absent;
    if(action==FingerprintRecoveryAction::FinishOldDeletion){
        for(unsigned i=0;i<recoveryRecord.job.deleteCount;++i)
            if(r.oldSlots[i]!=FingerprintSlotObservation::Absent)return false;
        return true;
    }
    return false;
}
void applyRecovery(const FingerprintRecoveryPlan &p){
    using R=FingerprintRecoveryResult;using A=FingerprintRecoveryAction;
    if(!FINGERPRINT_ENABLED){postRecoveryReceipt(R::Disabled,p.requestId);return;}
    if(!FP_RECOVERY_APPLY_ENABLED){postRecoveryReceipt(R::ApplyDisabled,p.requestId);return;}
    if(phase!=Phase::Done||command.operation!=FingerprintOperation::None){postRecoveryReceipt(R::Busy,p.requestId);return;}
    if(!recoveryHasReport || recoveryAwaitingAck || strcmp(p.ticket,recoveryReport.ticket) ||
       strcmp(p.journalId,recoveryReport.journalId) || millis()-recoveryReportedMs>=FP_RECOVERY_PLAN_TTL_MS ||
       !sameRecoveryJournal()){
        postRecoveryReceipt(R::StaleDecision,p.requestId);return;
    }
    if(p.action==A::Hold){recoveryHasReport=false;postRecoveryReceipt(R::Cancelled,p.requestId);return;}
    const auto op=recoveryRecord.job.operation;
    if((op==FingerprintOperation::Enroll && p.action!=A::KeepStoredTemplate&&p.action!=A::DiscardUncommittedTemplate)||
       (op==FingerprintOperation::DeleteOld&&p.action!=A::FinishOldDeletion)){
        postRecoveryReceipt(R::InvalidDecision,p.requestId);return;
    }
    recoveryHasReport=false;
    FingerprintRecoveryReport live{};
    if(!prepareRecoverySensor()||!observeJournal(live)){
        postRecoveryReceipt(recoveryStopped()?stoppedResult():R::SensorUnavailable,p.requestId,p.action);return;
    }
    if(!sameObservations(recoveryReport,live)||!protectedReadable(live)){
        postRecoveryReceipt(R::SensorChanged,p.requestId,p.action);return;
    }
    if(p.action==A::KeepStoredTemplate&&live.target!=FingerprintSlotObservation::PresentReadable){
        postRecoveryReceipt(R::VerificationFailed,p.requestId,p.action);return;
    }
    uint8_t removed=0;
    const unsigned count=p.action==A::FinishOldDeletion?recoveryRecord.job.deleteCount:
                         (p.action==A::DiscardUncommittedTemplate?1:0);
    for(unsigned i=0;i<count;++i){
        if(recoveryStopped()){postRecoveryReceipt(stoppedResult(),p.requestId,p.action,removed);return;}
        if(!sameRecoveryJournal()){postRecoveryReceipt(R::InvalidJournal,p.requestId,p.action,removed);return;}
        const int slot=p.action==A::FinishOldDeletion?recoveryRecord.job.deleteSlots[i]:recoveryRecord.job.targetSlot;
        bool used=false;
        uint8_t code=device.slotUsed(slot,used);
        if(code!=FINGERPRINT_OK){sensorReady=false;postRecoveryReceipt(R::SensorUnavailable,p.requestId,p.action,removed);return;}
        if(!used)continue; // An absent template needs no deletion.
        if(recoveryStopped()){postRecoveryReceipt(stoppedResult(),p.requestId,p.action,removed);return;}
        code=device.deleteModel(slot);
        if(code!=FINGERPRINT_OK){if(communicationError(code))sensorReady=false;postRecoveryReceipt(R::VerificationFailed,p.requestId,p.action,removed);return;}
        ++removed;
        code=device.slotUsed(slot,used);
        if(code!=FINGERPRINT_OK||used){postRecoveryReceipt(R::VerificationFailed,p.requestId,p.action,removed);return;}
    }
    FingerprintRecoveryReport after{};
    if(!observeJournal(after)||!desiredPostState(p.action,after)){
        postRecoveryReceipt(recoveryStopped()?stoppedResult():R::VerificationFailed,p.requestId,p.action,removed);return;
    }
    publish(FingerprintReaderState::RecoveryRequired);
    postRecoveryReceipt(R::AppliedAwaitingBackend,p.requestId,p.action,removed);
}
void finalizeRecovery(const FingerprintRecoveryAcknowledgement &ack){
    using R=FingerprintRecoveryResult;
    if(!FINGERPRINT_ENABLED||!FP_RECOVERY_APPLY_ENABLED){postRecoveryReceipt(R::ApplyDisabled,ack.requestId);return;}
    if(!recoveryAwaitingAck || strcmp(ack.receiptTicket,recoveryReceipt.ticket)||
       strcmp(ack.journalId,recoveryReceipt.journalId)||strcmp(ack.sessionId,recoveryReceipt.sessionId)||
       !sameRecoveryJournal()){
        postRecoveryReceipt(R::StaleDecision,ack.requestId);return;
    }
    if(phase!=Phase::Done||command.operation!=FingerprintOperation::None){postRecoveryReceipt(R::Busy,ack.requestId);return;}
    FingerprintRecoveryReport live{};
    if(!prepareRecoverySensor()||!observeJournal(live)||!desiredPostState(recoveryReceipt.action,live)){
        postRecoveryReceipt(recoveryStopped()?stoppedResult():R::VerificationFailed,ack.requestId);return;
    }
    // Clear the journal only after sensor verification and a durable server acknowledgement.
    if(recoveryStopped()){postRecoveryReceipt(stoppedResult(),ack.requestId);return;}
    if(!prefs.remove("pending") || prefs.getBytesLength("pending")!=0){
        postRecoveryReceipt(R::StorageFailed,ack.requestId);return;
    }
    journalActive=false;journalFromBoot=false;mutationComplete=false;
    recoveryHasReport=false;recoveryAwaitingAck=false;memset(&journal,0,sizeof(journal));
    postRecoveryReceipt(R::Cleared,ack.requestId,recoveryReceipt.action);
    publish(sensorReady?FingerprintReaderState::Ready:FingerprintReaderState::Unavailable);
}
bool processRecovery(){
    RecoveryMessage m{};
    if(xQueueReceive(recoveryQueue,&m,0)!=pdTRUE)return false;
    recoveryStartedMs=millis();
    // Check cancellation between UART calls.
    if(recoveryCancelled.exchange(false)){
        recoveryHasReport=false;recoveryAwaitingAck=false;
        postRecoveryReceipt(FingerprintRecoveryResult::Cancelled,m.requestId);return true;
    }
    if(m.kind==RecoveryMessageKind::Inspect)inspectRecovery(m.requestId);
    else if(m.kind==RecoveryMessageKind::Apply)applyRecovery(m.plan);
    else finalizeRecovery(m.acknowledgement);
    if(recoveryCancelled.load()){recoveryHasReport=false;recoveryAwaitingAck=false;}
    return true;
}

void tickWorker() {
    processAcks(); applyCommand();
    if(processRecovery())return;
    if (!FINGERPRINT_ENABLED) {
        publish(FingerprintReaderState::Disabled);
        if (phase!=Phase::Done) reject(FingerprintError::Disabled);
    } else if (phase==Phase::Done) {
        if (!journalActive) ensureSensor();
    } else if (millis()-operationStartedMs>=deadlineMs()) {
        reject(FingerprintError::TimedOut);
    } else if (journalFromBoot || (journalActive && phase==Phase::Start)) {
        reject(FingerprintError::RecoveryRequired);
    } else if (!ensureSensor()) {
        if (status.state==FingerprintReaderState::Unavailable) reject(FingerprintError::Unavailable);
        else if (status.state==FingerprintReaderState::RecoveryRequired) reject(FingerprintError::RecoveryRequired);
    } else if (stillCurrent()) stepOperation();
    if (pendingTerminal && xQueueSend(eventQueue,&terminal,0)==pdTRUE) pendingTerminal=false;
}
void workerTask(void *) {
    while (true) { tickWorker(); vTaskDelay(pdMS_TO_TICKS(FINGER_POLL_MS)); }
}
}

bool beginFingerprint() {
    using namespace fingerprint_detail;
    if (commandQueue || eventQueue || statusQueue || ackQueue || recoveryQueue || recoveryReportQueue || recoveryReceiptQueue) return false;
    commandQueue=xQueueCreate(1,sizeof(FingerprintCommand));
    eventQueue=xQueueCreate(8,sizeof(FingerprintEvent));
    statusQueue=xQueueCreate(1,sizeof(FingerprintStatus));
    ackQueue=xQueueCreate(2,sizeof(MutationAck));
    recoveryQueue=xQueueCreate(2,sizeof(RecoveryMessage));
    recoveryReportQueue=xQueueCreate(1,sizeof(FingerprintRecoveryReport));
    recoveryReceiptQueue=xQueueCreate(1,sizeof(FingerprintRecoveryReceipt));
    if (commandQueue && eventQueue && statusQueue && ackQueue && recoveryQueue && recoveryReportQueue && recoveryReceiptQueue) {
        publish(HardwareConfig::FINGERPRINT_ENABLED ? FingerprintReaderState::Starting : FingerprintReaderState::Disabled);
        if (xTaskCreate(workerTask,"Fingerprint",8192,nullptr,1,&taskHandle)==pdPASS) return true;
    }
    if (commandQueue) vQueueDelete(commandQueue);
    if (eventQueue) vQueueDelete(eventQueue);
    if (statusQueue) vQueueDelete(statusQueue);
    if (ackQueue) vQueueDelete(ackQueue);
    if(recoveryQueue)vQueueDelete(recoveryQueue);
    if(recoveryReportQueue)vQueueDelete(recoveryReportQueue);
    if(recoveryReceiptQueue)vQueueDelete(recoveryReceiptQueue);
    recoveryQueue=recoveryReportQueue=recoveryReceiptQueue=nullptr;
    commandQueue=nullptr; eventQueue=nullptr; statusQueue=nullptr; ackQueue=nullptr;
    return false;
}
bool requestFingerprint(const FingerprintCommand &c) {
    using namespace fingerprint_detail;
    if (!commandQueue || c.generation==0 ||
        static_cast<unsigned>(c.operation)>static_cast<unsigned>(FingerprintOperation::DeleteOld) ||
        !cString(c.sessionId,sizeof(c.sessionId)) || c.deleteCount>FP_MAX_SLOTS || c.protectedCount>FP_MAX_SLOTS)
        return false;
    return xQueueOverwrite(commandQueue,&c)==pdTRUE;
}
bool pollFingerprintEvent(FingerprintEvent &e) {
    return fingerprint_detail::eventQueue && xQueueReceive(fingerprint_detail::eventQueue,&e,0)==pdTRUE;
}
FingerprintStatus getFingerprintStatus() {
    FingerprintStatus s{};
    if (fingerprint_detail::statusQueue) xQueuePeek(fingerprint_detail::statusQueue,&s,0);
    return s;
}
bool acknowledgeFingerprintMutation(uint32_t generation,FingerprintOperation operation,const char *sessionId) {
    using namespace fingerprint_detail;
    if (!ackQueue || !generation || !sessionId ||
        (operation!=FingerprintOperation::Enroll && operation!=FingerprintOperation::DeleteOld)) return false;
    MutationAck a{}; a.generation=generation; a.operation=operation;
    const size_t n=strnlen(sessionId,sizeof(a.sessionId));
    if (!n || n>=sizeof(a.sessionId)) return false;
    memcpy(a.sessionId,sessionId,n+1);
    return xQueueSend(ackQueue,&a,0)==pdTRUE;
}
const char *fingerprintErrorText(FingerprintError e) {
    switch (e) {
    case FingerprintError::Disabled: return "Fingerprint hardware disabled";
    case FingerprintError::Unavailable: return "Fingerprint sensor unavailable";
    case FingerprintError::InvalidCommand: return "Invalid fingerprint operation";
    case FingerprintError::InvalidSlot: return "Invalid fingerprint slot";
    case FingerprintError::SlotOccupied: return "Assigned slot already occupied";
    case FingerprintError::IndexUnavailable: return "Cannot verify fingerprint slots";
    case FingerprintError::BadImage: return "Fingerprint image unclear";
    case FingerprintError::SamplesDiffer: return "Use the same finger twice";
    case FingerprintError::DuplicateFinger: return "Use a different backup finger";
    case FingerprintError::TimedOut: return "Fingerprint step timed out";
    case FingerprintError::SensorCommunication: return "Fingerprint communication error";
    case FingerprintError::StorageFailed: return "Fingerprint storage not confirmed";
    case FingerprintError::DeleteFailed: return "Fingerprint deletion not confirmed";
    case FingerprintError::JournalFailed: return "Cannot save fingerprint recovery record";
    case FingerprintError::RecoveryRequired: return "Fingerprint recovery required";
    default: return "Fingerprint operation failed";
    }
}

bool requestFingerprintRecoveryInspection(uint32_t requestId){
    using namespace fingerprint_detail;
    if(!recoveryQueue||!requestId)return false;
    RecoveryMessage m{};m.kind=RecoveryMessageKind::Inspect;m.requestId=requestId;
    return xQueueSend(recoveryQueue,&m,0)==pdTRUE;
}
bool pollFingerprintRecoveryReport(FingerprintRecoveryReport &report){
    using namespace fingerprint_detail;
    return recoveryReportQueue&&xQueueReceive(recoveryReportQueue,&report,0)==pdTRUE;
}
bool submitFingerprintRecoveryPlan(const FingerprintRecoveryPlan &p){
    using namespace fingerprint_detail;
    if(!recoveryQueue||!p.requestId||!cString(p.ticket,sizeof(p.ticket))||!p.ticket[0]||
       !cString(p.journalId,sizeof(p.journalId))||!p.journalId[0]||
       static_cast<unsigned>(p.action)>static_cast<unsigned>(FingerprintRecoveryAction::FinishOldDeletion))return false;
    RecoveryMessage m{};m.kind=RecoveryMessageKind::Apply;m.plan=p;m.requestId=p.requestId;
    return xQueueSend(recoveryQueue,&m,0)==pdTRUE;
}
bool pollFingerprintRecoveryReceipt(FingerprintRecoveryReceipt &receipt){
    using namespace fingerprint_detail;
    return recoveryReceiptQueue&&xQueueReceive(recoveryReceiptQueue,&receipt,0)==pdTRUE;
}
bool acknowledgeFingerprintRecovery(const FingerprintRecoveryAcknowledgement &a){
    using namespace fingerprint_detail;
    if(!recoveryQueue||!a.requestId||!cString(a.receiptTicket,sizeof(a.receiptTicket))||!a.receiptTicket[0]||
       !cString(a.journalId,sizeof(a.journalId))||!a.journalId[0]||
       !cString(a.sessionId,sizeof(a.sessionId))||!a.sessionId[0])return false;
    RecoveryMessage m{};m.kind=RecoveryMessageKind::Acknowledge;m.acknowledgement=a;m.requestId=a.requestId;
    return xQueueSend(recoveryQueue,&m,0)==pdTRUE;
}
void cancelFingerprintRecovery(){fingerprint_detail::recoveryCancelled.store(true);}
