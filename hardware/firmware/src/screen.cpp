// v8.2 + readable denial messages. Backend decisions and fixed deadlines unchanged.
// Hardware configuration, physical AS608 validation, and recovery gate are unchanged.
// The ID -> PIN step is LOCAL. The controller alone owns workflow and Nextion.
// RFID worker performs SPI reads; backend worker dispatches the existing hooks.
// Wi-Fi credentials stay in local secrets.h. Physical sensors/lock still default OFF.
#include <Arduino.h>
#include "BackendInterface.h"
#include "NetworkConfig.h"
#include "BackendPolicy.h"
#include "FailureDisplay.h"
#include "HardwareConfig.h"
#include "rfid.h"
#include "fingerprint.h"
#include "camera_module.h"
#include "lock.h"
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

namespace {
using namespace HardwareConfig;

// Names and lengths read from your nextionScreen.HMI.
constexpr const char *ID_INPUT = "t1";          // page1.t1, global
constexpr const char *ID_COUNTER = "va0";      // page1.va0.val
constexpr const char *ID_STATUS = "t2";         // page1.t2
constexpr const char *PIN_INPUT = "va0";       // page2/page8 real PIN text
constexpr const char *PIN_MASK = "t1";         // page2/page8 displayed asterisks
constexpr const char *PIN_COUNTER = "va1";     // page2/page8 digit count
constexpr const char *FAILURE_MESSAGE = "t1";  // page7.t1
constexpr size_t ID_DIGITS = 5;
constexpr size_t PIN_DIGITS = 4;

constexpr uint32_t ENTRY_TIMEOUT_MS = 60000;
constexpr uint32_t BACKEND_TIMEOUT_MS = NetworkConfig::CONTROLLER_TIMEOUT_MS;
constexpr uint32_t STEP_TIMEOUT_MS = 60000;
constexpr uint32_t SUCCESS_HOLD_MS = 3000;
constexpr uint32_t FAILURE_HOLD_MS = FailureDisplay::PAGE_HOLD_MS;
constexpr uint32_t ENROLLMENT_HOLD_MS = 2500;
constexpr uint32_t PARTIAL_LINE_TIMEOUT_MS = 1000;
constexpr size_t MAX_LINE_LENGTH = 95;
constexpr size_t MAX_DISPLAY_TEXT = 64;

// Original workflow states retained. ENROLLMENT_COMPLETE is new: a success
// screen is held without making the kiosk idle early.
enum KioskState {
    IDLE, ENTER_ID_PIN, ENTER_PIN, WAITING_FOR_BACKEND,
    AUTH_FACE, AUTH_FINGERPRINT,
    ENROLL_RFID, ENROLL_FACE, ENROLL_FINGERPRINT, REENROLL_RFID, REENROLL_FACE,
    REENROLL_FINGERPRINT, DELETE_OLD_FINGERPRINTS, UNLOCK_PENDING, ACCESS_GRANTED, ACCESS_DENIED,
    ENROLLMENT_COMPLETE
};

enum class BackendOperation : uint8_t {
    IdPin, RfidAuth, EnrollRfid, FingerprintAuth, EnrollFingerprint, ConfirmOldDeleted, FaceAuth, EnrollFace
};

struct BackendRequest {
    BackendOperation operation;
    uint32_t requestId;
    char employeeId[MAX_EMPLOYEE_ID_LENGTH + 1];
    char pin[MAX_PIN_LENGTH + 1];
    char rfidUid[RFID_UID_TEXT_CAPACITY];
    // Immutable context for this queued job; not references to UI globals.
    char flow[20];
    char credentialType[16];
    char enrollmentSessionId[MAX_SESSION_ID_LENGTH + 1];
    char authSessionId[MAX_SESSION_ID_LENGTH + 1];
    int templateSlot;
    uint32_t fingerprintGeneration;
    uint8_t deletionCount;
    CameraImageHandle image;
    uint32_t cameraGeneration;
    uint8_t captureIndex;
};
static_assert(std::is_trivially_copyable<BackendRequest>::value,
              "Backend requests must remain byte-copy safe");

HardwareSerial nextionSerial(2);
QueueHandle_t requestQueue = nullptr;
QueueHandle_t responseQueue = nullptr;
// Controller writes a length-one lease; backend reads copies to honor cancellation.
struct RequestLease {
    uint32_t requestId;
    uint32_t startedMs; // This individual request's budget.
    uint32_t workflowStartedMs;
    uint32_t workflowLifetimeMs; // 0 only for an initial, session-creating request.
};
QueueHandle_t requestLeaseQueue = nullptr;
// Backend worker owns this snapshot for the duration of one dispatched hook.
BackendRequestContext executingContext{};

TaskHandle_t uiTaskHandle = nullptr;
TaskHandle_t backendTaskHandle = nullptr;

// ONLY the UI/controller task reads/writes these fields.
KioskState currentState = IDLE;
BackendResponse session{};
// The server does not yet send an expiry field. Start conservatively from the
// queue time of the request that CREATES the session, before server creation.
// Its queue/transit/processing time is deducted; receiving a reply never refreshes
// this deadline. This may expire early, and is not a synchronized server clock.
BackendPolicy::SessionDeadline workflowDeadline{};
uint32_t requestCounter = 0;
uint32_t activeRequestId = 0;
BackendOperation activeOperation = BackendOperation::IdPin;
BackendRequest activeJob{};
uint32_t fingerprintGeneration = 0;
bool fingerprintCommandAccepted = false;
uint32_t cameraGeneration = 0;
bool cameraCommandAccepted = false, cameraCaptureRequested = false;
int lastCameraCountdown = -1;
uint8_t acknowledgedFaceCount = 0;
// Read/written ONLY by backend task; public accessor checks task identity first.
CameraImageView backendFaceView{};
uint32_t backendFaceRequestId = 0;
int acknowledgedFingerprintSlots[2] = {-1,-1};
uint8_t acknowledgedFingerprintCount = 0;
static_assert(MAX_SESSION_ID_LENGTH + 1 == FP_SESSION_CAPACITY, "Session capacity mismatch");
static_assert(MAX_OLD_FINGERPRINT_SLOTS == FP_MAX_SLOTS, "Slot capacity mismatch");
uint32_t rfidGeneration = 0;
bool rfidCommandAccepted = false;
bool startIdAfterFailure = false;
uint32_t stateStartedMs = 0;
uint32_t expectedLockPulse = 0;
const char *displayedPage = "";
char currentEmployeeId[MAX_EMPLOYEE_ID_LENGTH + 1] = {};
FailureDisplay::Pages failurePages{};
size_t failurePageIndex = 0;

bool equalText(const char *a, const char *b) { return strcmp(a, b) == 0; }

template <size_t N>
bool terminated(const char (&text)[N]) {
    return memchr(text, '\0', N) != nullptr;
}

// These original helper NAMES are retained, but are private to the UI task.
// Backend code must call handleBackendResponse(), never these helpers.
void sendNextion(String command) {
    configASSERT(xTaskGetCurrentTaskHandle() == uiTaskHandle);
    nextionSerial.print(command);
    const uint8_t terminator[] = {0xFF, 0xFF, 0xFF};
    nextionSerial.write(terminator, sizeof(terminator));
}

void setText(const char *component, const char *text) {
    String safeText;
    safeText.reserve(MAX_DISPLAY_TEXT);
    for (size_t i = 0; text[i] != '\0' && i < MAX_DISPLAY_TEXT; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        // Do not let quotes/backslashes/control bytes escape a Nextion string.
        safeText += (c >= 32 && c <= 126 && c != '"' && c != '\\')
                    ? static_cast<char>(c) : ' ';
    }
    sendNextion(String(component) + ".txt=\"" + safeText + "\"");
}

void clearCurrentPin() {
    // These variables are local: clear them BEFORE leaving their page.
    if (equalText(displayedPage, "page2") || equalText(displayedPage, "page8")) {
        setText(PIN_INPUT, "");
        setText(PIN_MASK, "");
        sendNextion(String(PIN_COUNTER) + ".val=0");
    }
}

void showPage(const char *page) {
    clearCurrentPin();
    sendNextion(String("page ") + page);
    displayedPage = page;  // All callers pass persistent string literals.
}

void showIdleScreen() { showPage("page0"); }
void showIdPinScreen() {
    // This existing helper now opens the actual ID page; PIN is on page2.
    showPage("page1");
    setText(ID_INPUT, "");
    sendNextion(String(ID_COUNTER) + ".val=0");
    setText(ID_STATUS, "");
    sendNextion("tsw b0,1");
}
void showPinScreen() {
    // Keep the format-checked ID available to page2's AUTH event.
    // This is NOT confirmation that the ID exists or is authorized.
    setText("page1.t1", currentEmployeeId);
    showPage("page2");  // Same PIN page for all three backend workflows.
    clearCurrentPin();
    sendNextion("tsw b0,1");
}
void showRfidScreen() { showPage("page9"); }
const char *const facePromptCodes[] = {"LOOK_STRAIGHT", "TURN_SLIGHTLY_LEFT", "TURN_MORE_LEFT",
                                      "TURN_SLIGHTLY_RIGHT", "TURN_MORE_RIGHT"};
const char *const facePromptText[] = {"Look straight at camera", "Turn slightly left", "Turn more left",
                                    "Turn slightly right", "Turn more right"};
const char *const facePages[] = {"page10", "page11", "page12", "page13", "page14"};
int facePromptIndex(const char *code) {
    for(int i=0;i<5;++i)if(equalText(code,facePromptCodes[i]))return i;
    return -1;
}
void showFaceScreen() {
    if(currentState==AUTH_FACE) {
        showPage("page4"); sendNextion("tm0.en=0");
        char countdown[32];
        snprintf(countdown,sizeof(countdown),"n0.val=%lu",static_cast<unsigned long>((CAMERA_POSITION_DELAY_MS+999)/1000));
        sendNextion(countdown); setText("t0","Look at camera");
    } else {
        // Router validates count and supplied prompt. Omitted prompt uses the
        // agreed five-position sequence, not an invented workflow transition.
        const unsigned index=acknowledgedFaceCount<5?acknowledgedFaceCount:0;
        showPage(facePages[index]);
        char title[40]; snprintf(title,sizeof(title),"Face capture %u/5",index+1);
        setText("t0",title); setText("t1",facePromptText[index]);
    }
}
void showFingerprintScreen() { showPage("page15"); } // Enrollment
void showSuccessScreen() {
    showPage("page6");
    sendNextion("tm0.en=0"); // Controller owns the result hold time.
}
void renderFailurePage(size_t index) {
    if (index >= failurePages.count) return;
    char escaped[FailureDisplay::COMMAND_TEXT_BYTES] = {};
    if (!FailureDisplay::encode(failurePages.text[index], escaped, sizeof(escaped))) {
        setText(FAILURE_MESSAGE, "Cannot display error details");
        return;
    }
    sendNextion(String(FAILURE_MESSAGE) + ".txt=\"" + escaped + "\"");
    failurePageIndex = index;
}
void showFailureScreen(String reason) {
    // Presentation only: preserve the raw reason for controller routing elsewhere.
    failurePages = FailureDisplay::format(reason.c_str());
    failurePageIndex = 0;
    showPage("page7");
    sendNextion("tm0.en=0"); // Controller owns the hold time and page changes.
    sendNextion("t1.font=5"); // Arial16 in the supplied Nextion project.
    sendNextion("t1.xcen=0");
    sendNextion("t1.ycen=0");
    sendNextion("t1.spax=0");
    sendNextion("t1.spay=0");
    renderFailurePage(0);
}

void changeState(KioskState state) {
    currentState = state;
    stateStartedMs = millis();
    if (requestLeaseQueue) {
        const RequestLease lease{state == WAITING_FOR_BACKEND ? activeRequestId : 0,
                                 stateStartedMs, workflowDeadline.startedMs,
                                 workflowDeadline.lifetimeMs};
        xQueueOverwrite(requestLeaseQueue, &lease);
    }
    if (++rfidGeneration == 0) ++rfidGeneration;
    RfidScanMode mode = RfidScanMode::Disabled;
    if (state == IDLE) mode = RfidScanMode::Authentication;
    else if (state == ENROLL_RFID || state == REENROLL_RFID)
        mode = RfidScanMode::Enrollment;
    // Each state entry invalidates queued scans from the previous state.
    rfidCommandAccepted = setRFIDScanMode(mode, rfidGeneration);

    if (++fingerprintGeneration == 0) ++fingerprintGeneration;
    FingerprintCommand job{};
    job.generation = fingerprintGeneration;
    if (state == AUTH_FINGERPRINT) {
        job.operation = FingerprintOperation::Match;
        copyBackendText(job.sessionId, session.authSessionId);
    } else if (state == ENROLL_FINGERPRINT || state == REENROLL_FINGERPRINT ||
               state == DELETE_OLD_FINGERPRINTS) {
        job.operation = state == DELETE_OLD_FINGERPRINTS ? FingerprintOperation::DeleteOld
                                                       : FingerprintOperation::Enroll;
        copyBackendText(job.sessionId, session.enrollmentSessionId);
        job.targetSlot = session.fingerprintSlot;
        job.protectedCount = acknowledgedFingerprintCount;
        for (uint8_t i=0; i<acknowledgedFingerprintCount; ++i)
            job.protectedSlots[i] = acknowledgedFingerprintSlots[i];
        if (state == DELETE_OLD_FINGERPRINTS) {
            job.deleteCount = session.oldFingerprintSlotCount;
            for (uint8_t i=0; i<job.deleteCount; ++i) job.deleteSlots[i] = session.oldFingerprintSlots[i];
        }
    }
    // None cancels capture without undoing physical writes or clearing journals.
    fingerprintCommandAccepted = requestFingerprint(job);

    if(++cameraGeneration==0)++cameraGeneration;
    CameraCommand cancel{}; cancel.generation=cameraGeneration;
    cameraCommandAccepted=requestCameraCapture(cancel);
    cameraCaptureRequested=false; lastCameraCountdown=-1;
    // The actual capture command is armed by checkCameraStatus AFTER positioning.
}

void resetSession() {
    // Invalidate queued/in-progress network work before any slow screen writes.
    if (requestLeaseQueue) {
        const RequestLease cancelled{0, millis(), 0, 0};
        xQueueOverwrite(requestLeaseQueue, &cancelled);
    }
    // A cancelled/restarted UI may not leave an accepted pulse waiting to start.
    if (!lockNow()) Serial.println("[Lock] Could not confirm locked GPIO command.");
    expectedLockPulse = 0;
    // Safe on cancel: ready copies are freed, but borrowed upload memory is not.
    discardCameraImage(activeJob.image);
    BackendRequest dropped{};
    while(requestQueue && xQueueReceive(requestQueue,&dropped,0)==pdTRUE) {
        discardCameraImage(dropped.image);
        memset(&dropped,0,sizeof(dropped));
    }
    acknowledgedFaceCount=0;
    workflowDeadline.clear();
    session = BackendResponse{};
    activeRequestId = 0;
    activeOperation = BackendOperation::IdPin;
    activeJob = BackendRequest{};
    acknowledgedFingerprintCount = 0;
    acknowledgedFingerprintSlots[0] = acknowledgedFingerprintSlots[1] = -1;
    startIdAfterFailure = false;
    memset(currentEmployeeId, 0, sizeof(currentEmployeeId));
    // These four text objects are GLOBAL in the uploaded HMI.
    setText("page1.t1", "");
    setText("page1.t2", "");
    setText("page2.t1", "");
    setText("page8.t1", "");
    // Already-running backend work cannot be undone; its stale result is ignored.
}

void returnToIdle() {
    resetSession();
    changeState(IDLE);
    showIdleScreen();
}

void fail(const char *message) {
    // Request locked output BEFORE potentially slow screen writes.
    if (!lockNow()) Serial.println("[Lock] Locked GPIO command failed.");
    // Show first: message might refer to data that resetSession() clears.
    showFailureScreen(String(message));
    resetSession();
    changeState(ACCESS_DENIED);
}

bool expireWorkflowIfNeeded() {
    if (!workflowDeadline.expired(millis())) return false;
    const bool authentication = workflowDeadline.lifetimeMs == BackendPolicy::AUTH_SESSION_MS;
    // Cancel workers and invalidate the backend lease BEFORE any screen writes.
    // None cancels future work; it cannot undo a store/delete already issued.
    changeState(ACCESS_DENIED);
    fail(authentication ? "Authentication session expired" : "Enrollment session expired");
    return true;
}

bool validFields(const BackendResponse &r) {
    return terminated(r.flow) && terminated(r.nextStep) &&
           terminated(r.credentialType) && terminated(r.authSessionId) &&
           terminated(r.enrollmentSessionId) && terminated(r.failureReason) &&
           (r.fingerprintSlot == -1 || BackendPolicy::validFingerprintSlot(r.fingerprintSlot)) &&
           r.fingerprintsEnrolled >= -1 && r.fingerprintsEnrolled <= 2 &&
           (r.fingerprintsRequired == -1 || r.fingerprintsRequired == 2) &&
           terminated(r.nextPrompt) && r.capturesCompleted>=-1 && r.capturesCompleted<=5 &&
           (r.capturesRequired==-1 || r.capturesRequired==5) &&
           (r.nextCaptureIndex==-1 || (r.nextCaptureIndex>=1 && r.nextCaptureIndex<=5));
}

bool validOldSlots(const BackendResponse &r) {
    if (!r.hasOldFingerprintSlots ||
        r.oldFingerprintSlotCount > MAX_OLD_FINGERPRINT_SLOTS) return false;
    for (uint8_t i = 0; i < r.oldFingerprintSlotCount; ++i) {
        if (!BackendPolicy::validFingerprintSlot(r.oldFingerprintSlots[i])) return false;
        for (uint8_t j = 0; j < i; ++j) {
            if (r.oldFingerprintSlots[i] == r.oldFingerprintSlots[j]) return false;
        }
    }
    return true;
}

void applyBackendResponse(const BackendResponse &r) {
    // A cancelled, timed-out, duplicated, or unrelated response cannot change UI.
    if (currentState != WAITING_FOR_BACKEND || r.requestId == 0 ||
        r.requestId != activeRequestId) return;
    if (expireWorkflowIfNeeded()) return;
    if (millis() - stateStartedMs >= BACKEND_TIMEOUT_MS) {
        fail("Backend timed out");
        return;
    }
    if (!validFields(r)) { fail("Invalid backend response"); return; }
    if (!r.success) {
        // Contract: RFID with pending credential replacement goes to ID/PIN.
        // A future HTTP adapter must retain this exact reason code for routing.
        const bool needsId = activeOperation == BackendOperation::RfidAuth &&
                             equalText(r.failureReason, "REENROLLMENT_REQUIRED");
        fail(needsId ? "Credential update required" :
             (r.failureReason[0] ? r.failureReason : "Request rejected"));
        startIdAfterFailure = needsId;
        return;
    }

    const bool auth = equalText(r.flow, "AUTHENTICATION");
    const bool enroll = equalText(r.flow, "ENROLLMENT");
    const bool reenroll = equalText(r.flow, "REENROLLMENT");
    if ((!auth && !enroll && !reenroll) ||
        (auth && r.authSessionId[0] == '\0') ||
        (!auth && r.enrollmentSessionId[0] == '\0')) {
        fail("Invalid workflow or session");
        return;
    }
    if (!workflowDeadline.active()) {
        const bool initial = activeOperation == BackendOperation::IdPin ||
                             activeOperation == BackendOperation::RfidAuth;
        if (!initial || session.flow[0] != '\0') {
            fail("Missing whole-session deadline"); return;
        }
        workflowDeadline.begin(auth, stateStartedMs);
    }
    if (expireWorkflowIfNeeded()) return;
    if (activeOperation == BackendOperation::RfidAuth && !auth) {
        fail("Invalid RFID authentication flow");
        return;
    }
    // Internal normalized response, NOT a verbatim JSON object.
    // A future HTTP adapter must carry forward optional workflow/session fields
    // from the ORIGINAL request's saved context, never another active user's.
    // Initial ID/PIN or RFID-auth requests have no prior session to inherit.
    // The v8 HTTP adapter rejects contradictory request/workflow/session identities.
    if (session.flow[0] != '\0' &&
        (!equalText(session.flow, r.flow) ||
         (auth && !equalText(session.authSessionId, r.authSessionId)) ||
         (!auth && !equalText(session.enrollmentSessionId, r.enrollmentSessionId)))) {
        fail("Session mismatch");
        return;
    }
    if (r.credentialType[0] != '\0' &&
        !equalText(r.credentialType, "RFID") &&
        !equalText(r.credentialType, "FACE") &&
        !equalText(r.credentialType, "FINGERPRINT")) {
        fail("Invalid credential type");
        return;
    }

    if (session.credentialType[0] && r.credentialType[0] &&
        !equalText(session.credentialType, r.credentialType)) {
        fail("Credential mismatch");
        return;
    }
    // Only steps supported by the agreed RFID endpoint may follow its request.
    if (activeOperation == BackendOperation::EnrollRfid &&
        !((enroll && equalText(r.nextStep, "FACE")) ||
          (reenroll && equalText(session.credentialType, "RFID") &&
           equalText(r.nextStep, "COMPLETE")))) {
        fail("Unexpected RFID enrollment step");
        return;
    }

    uint8_t nextFaceCount=acknowledgedFaceCount;
    if(activeOperation==BackendOperation::FaceAuth) {
        if(!auth || !activeJob.image || !equalText(r.nextStep,"COMPLETE")) {
            fail("Invalid face authentication approval"); return;
        }
    }
    if(activeOperation==BackendOperation::EnrollFace) {
        if(auth || !activeJob.image || acknowledgedFaceCount>=5 ||
           activeJob.captureIndex!=acknowledgedFaceCount+1 ||
           (reenroll && !equalText(r.credentialType,"FACE"))) {
            fail("Invalid face enrollment response"); return;
        }
        nextFaceCount=acknowledgedFaceCount+1;
        const char *expected=nextFaceCount<5?"FACE":(reenroll?"COMPLETE":"FINGERPRINT");
        if(!equalText(r.nextStep,expected) ||
           (r.capturesCompleted>=0 && r.capturesCompleted!=nextFaceCount)) {
            fail("Unexpected face enrollment step"); return;
        }
    }
    if(!auth && equalText(r.nextStep,"FACE")) {
        if(nextFaceCount>=5 || (r.capturesCompleted>=0 && r.capturesCompleted!=nextFaceCount) ||
           (r.nextCaptureIndex>=0 && r.nextCaptureIndex!=nextFaceCount+1) ||
           (r.nextPrompt[0] && facePromptIndex(r.nextPrompt)!=nextFaceCount)) {
            fail("Face capture progress or prompt mismatch"); return;
        }
    }

    // Hardware completion is NOT server acknowledgement. All destructive steps
    // below must belong to a locally completed, correlated job in this session.
    uint8_t nextFingerprintCount = acknowledgedFingerprintCount;
    if (activeOperation == BackendOperation::FingerprintAuth) {
        if (!auth || !BackendPolicy::validFingerprintSlot(activeJob.templateSlot) || !equalText(r.nextStep,"COMPLETE")) {
            fail("Invalid fingerprint approval"); return;
        }
    }
    if (activeOperation == BackendOperation::EnrollFingerprint) {
        if (auth || !BackendPolicy::validFingerprintSlot(activeJob.templateSlot) || acknowledgedFingerprintCount >= 2 ||
            (reenroll && !equalText(r.credentialType,"FINGERPRINT"))) {
            fail("Invalid fingerprint enrollment response"); return;
        }
        nextFingerprintCount = acknowledgedFingerprintCount + 1;
        const char *expected = nextFingerprintCount == 1 ? "FINGERPRINT" :
                               (reenroll ? "DELETE_OLD_FINGERPRINTS" : "COMPLETE");
        if (!equalText(r.nextStep, expected) ||
            (r.fingerprintsEnrolled >= 0 && r.fingerprintsEnrolled != nextFingerprintCount)) {
            fail("Unexpected fingerprint enrollment step"); return;
        }
        if (nextFingerprintCount == 1 && r.fingerprintSlot == activeJob.templateSlot) {
            fail("Backend reused fingerprint slot"); return;
        }
    }
    if (activeOperation == BackendOperation::ConfirmOldDeleted &&
        (!reenroll || !equalText(r.credentialType,"FINGERPRINT") ||
         !equalText(r.nextStep,"COMPLETE") || acknowledgedFingerprintCount != 2)) {
        fail("Unexpected fingerprint deletion response"); return;
    }
    if (equalText(r.nextStep,"DELETE_OLD_FINGERPRINTS")) {
        if (activeOperation != BackendOperation::EnrollFingerprint || !reenroll ||
            nextFingerprintCount != 2 || !validOldSlots(r)) {
            fail("Replacements must be enrolled first"); return;
        }
        for (uint8_t i=0; i<r.oldFingerprintSlotCount; ++i) {
            if (r.oldFingerprintSlots[i] == activeJob.templateSlot) {
                fail("Deletion includes a new template"); return;
            }
            for (uint8_t j=0; j<acknowledgedFingerprintCount; ++j)
                if (r.oldFingerprintSlots[i] == acknowledgedFingerprintSlots[j]) {
                    fail("Deletion includes a new template"); return;
                }
        }
    }
    if (equalText(r.nextStep,"FINGERPRINT") && !auth &&
        r.fingerprintsEnrolled >= 0 && r.fingerprintsEnrolled != nextFingerprintCount) {
        // No automatic resume of a previous boot's partially enrolled session.
        fail("Fingerprint session needs reconciliation"); return;
    }

    // ID/PIN and RFID-first requests only START a workflow. Neither may skip
    // biometrics or order deletion of templates before replacements exist.
    if (session.flow[0] == '\0' &&
        (equalText(r.nextStep, "COMPLETE") ||
         equalText(r.nextStep, "DELETE_OLD_FINGERPRINTS"))) {
        fail("Unexpected initial step");
        return;
    }

    KioskState destination = IDLE;  // IDLE is not a valid successful next step.
    if (equalText(r.nextStep, "COMPLETE")) {
        destination = auth ? UNLOCK_PENDING : ENROLLMENT_COMPLETE;
    } else if (auth) {
        if (equalText(r.nextStep, "FACE")) destination = AUTH_FACE;
        else if (equalText(r.nextStep, "FINGERPRINT")) destination = AUTH_FINGERPRINT;
    } else if (enroll) {
        if (equalText(r.nextStep, "RFID")) destination = ENROLL_RFID;
        else if (equalText(r.nextStep, "FACE")) destination = ENROLL_FACE;
        else if (equalText(r.nextStep, "FINGERPRINT")) destination = ENROLL_FINGERPRINT;
    } else if (equalText(r.nextStep, "DELETE_OLD_FINGERPRINTS")) {
        if (equalText(r.credentialType, "FINGERPRINT") && validOldSlots(r))
            destination = DELETE_OLD_FINGERPRINTS;
    } else if (equalText(r.nextStep, r.credentialType)) {
        if (equalText(r.nextStep, "RFID")) destination = REENROLL_RFID;
        else if (equalText(r.nextStep, "FACE")) destination = REENROLL_FACE;
        else if (equalText(r.nextStep, "FINGERPRINT")) destination = REENROLL_FINGERPRINT;
    }
    if (destination == IDLE) { fail("Invalid backend next step"); return; }
    if ((destination == ENROLL_FINGERPRINT || destination == REENROLL_FINGERPRINT)
        && !BackendPolicy::validFingerprintSlot(r.fingerprintSlot)) {
        fail("Missing or invalid fingerprint slot");
        return;
    }
    // Recheck before mutation acknowledgement, starting another worker, or unlock.
    // An expired reply must not clear a pending fingerprint journal.
    if (expireWorkflowIfNeeded()) return;
    // Slot capacity/occupancy is validated independently inside the AS608 worker.
    if (activeOperation == BackendOperation::EnrollFingerprint ||
        (activeOperation == BackendOperation::ConfirmOldDeleted && activeJob.deletionCount > 0)) {
        const FingerprintOperation op = activeOperation == BackendOperation::EnrollFingerprint ?
            FingerprintOperation::Enroll : FingerprintOperation::DeleteOld;
        if (!acknowledgeFingerprintMutation(activeJob.fingerprintGeneration,op,
                                            activeJob.enrollmentSessionId)) {
            fail("Cannot acknowledge fingerprint change"); return;
        }
    }
    if (activeOperation == BackendOperation::EnrollFingerprint) {
        acknowledgedFingerprintSlots[acknowledgedFingerprintCount] = activeJob.templateSlot;
        acknowledgedFingerprintCount = nextFingerprintCount;
    }
    if (destination == UNLOCK_PENDING) {
        if (activeOperation != BackendOperation::FaceAuth &&
            activeOperation != BackendOperation::FingerprintAuth) {
            fail("Biometric approval required"); return;
        }
        // Only an authenticated, correlated biometric completion can reach here.
        // Initial ID/PIN/RFID COMPLETE and enrollment COMPLETE cannot energize GPIO.
        if (!LOCK_ENABLED) { fail("Auth OK; lock output disabled"); return; }
        if (!triggerLock(static_cast<int>(LOCK_PULSE_MS))) {
            fail("Auth OK; lock unavailable"); return;
        }
        expectedLockPulse = getLockStatus().pulseNumber;
    }
    // Final approval ends the workflow, not the lock pulse. Its independent
    // output deadline remains unchanged. Enrollment completion never unlocks.
    if (destination == UNLOCK_PENDING || destination == ENROLLMENT_COMPLETE)
        workflowDeadline.clear();
    acknowledgedFaceCount=nextFaceCount;
    session = r;  // No inheritance of old target slots or deletion lists.
    activeRequestId = 0;
    changeState(destination);
    switch (destination) {
        case UNLOCK_PENDING:
            // Existing Verifying page remains for the short worker-start interval.
            // Do not display access success just because triggerLock() accepted.
            showPage("page3"); break;
        case ENROLLMENT_COMPLETE:
            showPage("page17");
            sendNextion("tm0.en=0");
            break;
        case AUTH_FACE: showFaceScreen(); break;
        case AUTH_FINGERPRINT:
            showPage("page5"); setText("t0","Remove finger to start"); break;
        case ENROLL_FINGERPRINT:
        case REENROLL_FINGERPRINT:
            showPage(acknowledgedFingerprintCount == 0 ? "page15" : "page16");
            setText("t1","Remove finger to start");
            break;
        case DELETE_OLD_FINGERPRINTS:
            showPage("page15"); setText("t0","Update fingerprints");
            setText("t1","Deleting old templates..."); break;
        case ENROLL_RFID:
        case REENROLL_RFID: showRfidScreen(); break;
        case ENROLL_FACE:
        case REENROLL_FACE: showFaceScreen(); break;
        default: showFingerprintScreen(); break;
    }
    // Camera begins after positioning; each next capture needs server ACK.
    // Enrollment completion never operates a lock.
}

bool exactDigits(const char *text, size_t count) {
    if (strlen(text) != count) return false;
    for (size_t i = 0; i < count; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
    }
    return true;
}

void queueBackendRequest(BackendRequest &request) {
    const bool initial = request.operation == BackendOperation::IdPin ||
                         request.operation == BackendOperation::RfidAuth;
    if (expireWorkflowIfNeeded() || (!initial && !workflowDeadline.active())) {
        discardCameraImage(request.image);
        memset(&request, 0, sizeof(request));
        if (currentState != ACCESS_DENIED) fail("Missing whole-session deadline");
        return;
    }
    if (initial && workflowDeadline.active()) {
        discardCameraImage(request.image);
        memset(&request, 0, sizeof(request));
        fail("A workflow is already active"); return;
    }
    if (++requestCounter == 0) ++requestCounter;
    request.requestId = requestCounter;
    activeRequestId = request.requestId;
    activeOperation = request.operation;
    activeJob = request; // Immutable request snapshot for correlation and mutation ACKs.
    changeState(WAITING_FOR_BACKEND); // Also disarms RFID scans.
    // showPage clears a real PIN before leaving page2.
    showPage("page3");
    if (xQueueSend(requestQueue, &request, 0) != pdTRUE) {
        discardCameraImage(request.image); fail("Backend worker busy");
    }
    memset(&request, 0, sizeof(request)); // Best-effort cleanup, not secure erasure.
}

void submitIdPinRequest(const char *pin) {
    BackendRequest request{};
    request.operation = BackendOperation::IdPin;
    if (!copyBackendText(request.employeeId, currentEmployeeId) ||
        !copyBackendText(request.pin, pin)) {
        fail("Input is too long");
        return;
    }
    queueBackendRequest(request);
}

bool validUidText(const RfidEvent &event) {
    if ((event.uidLength != 4 && event.uidLength != 7 && event.uidLength != 10) ||
        !terminated(event.uid) || strlen(event.uid) != event.uidLength * 2U)
        return false;
    for (const char *p = event.uid; *p; ++p) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F'))) return false;
    }
    return true;
}

void processRFIDEvent(const RfidEvent &event) {
    if (expireWorkflowIfNeeded()) return;
    const bool initialScan = currentState == IDLE &&
                             event.mode == RfidScanMode::Authentication;
    const bool enrollmentScan = (currentState == ENROLL_RFID ||
                                 currentState == REENROLL_RFID) &&
                                event.mode == RfidScanMode::Enrollment;
    if (!rfidCommandAccepted || event.generation == 0 ||
        event.generation != rfidGeneration || (!initialScan && !enrollmentScan))
        return; // Wrong workflow, cancelled operation, or stale hardware result.
    if (enrollmentScan && millis() - stateStartedMs >= STEP_TIMEOUT_MS) {
        fail("Step timed out");
        return;
    }
    if (event.type == RfidEventType::ReadError) {
        fail("RFID read error. Use one card");
        return;
    }
    if (event.type != RfidEventType::CardScanned || !validUidText(event)) {
        fail("Invalid RFID data");
        return;
    }
    BackendRequest request{};
    request.operation = initialScan ? BackendOperation::RfidAuth
                                    : BackendOperation::EnrollRfid;
    if (!copyBackendText(request.rfidUid, event.uid)) {
        fail("Invalid RFID UID");
        return;
    }
    if (enrollmentScan) {
        if (!session.enrollmentSessionId[0] ||
            !copyBackendText(request.enrollmentSessionId, session.enrollmentSessionId) ||
            !copyBackendText(request.flow, session.flow) ||
            !copyBackendText(request.credentialType, session.credentialType)) {
            fail("Missing enrollment context");
            return;
        }
    }
    // No local card whitelist; a UID is not an access-granted decision.
    queueBackendRequest(request);
}

bool fingerprintState() {
    return currentState == AUTH_FINGERPRINT || currentState == ENROLL_FINGERPRINT ||
           currentState == REENROLL_FINGERPRINT || currentState == DELETE_OLD_FINGERPRINTS;
}
void displayFingerprintProgress(FingerprintPrompt prompt) {
    const char *text = "Processing fingerprint...";
    switch (prompt) {
    case FingerprintPrompt::RemoveFinger: text="Remove finger"; break;
    case FingerprintPrompt::PlaceFinger:
        text=(currentState != AUTH_FINGERPRINT && acknowledgedFingerprintCount == 1)
             ? "Place a different finger" : "Place finger"; break;
    case FingerprintPrompt::PlaceSameFinger: text="Place the SAME finger again"; break;
    case FingerprintPrompt::Storing: text="Saving fingerprint..."; break;
    case FingerprintPrompt::Deleting: text="Deleting old templates..."; break;
    case FingerprintPrompt::PoorImage: text="Unclear image. Remove finger, retry"; break;
    case FingerprintPrompt::UseDifferentFinger: text="Use a DIFFERENT backup finger"; break;
    default: break;
    }
    // Actual original HMI: page5 has t0 (NOT t1), pages15/16 have t1.
    setText(currentState == AUTH_FINGERPRINT ? "t0" : "t1", text);
}
void processFingerprintEvent(const FingerprintEvent &event) {
    if (expireWorkflowIfNeeded()) return;
    if (!fingerprintState() || !fingerprintCommandAccepted || !event.generation ||
        event.generation != fingerprintGeneration) return;
    const FingerprintOperation expected = currentState == AUTH_FINGERPRINT ? FingerprintOperation::Match :
        (currentState == DELETE_OLD_FINGERPRINTS ? FingerprintOperation::DeleteOld : FingerprintOperation::Enroll);
    if (event.operation != expected) return;
    if (event.type == FingerprintEventType::Progress) { displayFingerprintProgress(event.prompt); return; }
    if (event.type == FingerprintEventType::Failed) { fail(fingerprintErrorText(event.error)); return; }
    BackendRequest job{};
    job.fingerprintGeneration=event.generation;
    if (!copyBackendText(job.flow,session.flow) ||
        !copyBackendText(job.credentialType,session.credentialType)) { fail("Missing fingerprint context"); return; }
    if (expected == FingerprintOperation::Match &&
        (event.type == FingerprintEventType::Matched || event.type == FingerprintEventType::NoMatch)) {
        if (!session.authSessionId[0] || !copyBackendText(job.authSessionId,session.authSessionId) ||
            (event.type == FingerprintEventType::Matched && !BackendPolicy::validFingerprintSlot(event.slot))) { fail("Invalid fingerprint result"); return; }
        job.operation=BackendOperation::FingerprintAuth;
        // Future HTTP adapter MUST encode -1 as JSON null, never send slot=-1.
        job.templateSlot=event.type == FingerprintEventType::NoMatch ? -1 : event.slot;
    } else if (expected == FingerprintOperation::Enroll && event.type == FingerprintEventType::Enrolled) {
        if (event.slot != session.fingerprintSlot || !BackendPolicy::validFingerprintSlot(event.slot) ||
            !session.enrollmentSessionId[0] ||
            !copyBackendText(job.enrollmentSessionId,session.enrollmentSessionId)) {
            fail("Unexpected fingerprint storage slot"); return;
        }
        job.operation=BackendOperation::EnrollFingerprint; job.templateSlot=event.slot;
    } else if (expected == FingerprintOperation::DeleteOld && event.type == FingerprintEventType::OldDeleted) {
        if (!validOldSlots(session) || event.deletedCount != session.oldFingerprintSlotCount ||
            acknowledgedFingerprintCount != 2 || !session.enrollmentSessionId[0] ||
            !copyBackendText(job.enrollmentSessionId,session.enrollmentSessionId)) {
            fail("Fingerprint deletion incomplete"); return;
        }
        job.operation=BackendOperation::ConfirmOldDeleted; job.deletionCount=event.deletedCount;
    } else { fail("Invalid fingerprint event"); return; }
    queueBackendRequest(job); // Disarms sensor workers; preserves this job's generation.
}
void checkFingerprintStatus() {
    const FingerprintStatus status=getFingerprintStatus();
    static int lastLoggedState=-1;
    if (lastLoggedState != static_cast<int>(status.state)) {
        lastLoggedState=static_cast<int>(status.state);
        switch (status.state) {
        case FingerprintReaderState::Disabled: Serial.println("[Fingerprint] Hardware disabled in HardwareConfig.h."); break;
        case FingerprintReaderState::Starting: Serial.println("[Fingerprint] Starting UART worker."); break;
        case FingerprintReaderState::Ready: Serial.println("[Fingerprint] Sensor communication/parameters checked."); break;
        case FingerprintReaderState::RecoveryRequired: Serial.println("[Fingerprint] Reconciliation required; do not erase recovery record."); break;
        default: Serial.println("[Fingerprint] Sensor unavailable."); break;
        }
    }
    if (!fingerprintState()) return;
    if (!fingerprintCommandAccepted) fail("Fingerprint worker unavailable");
    else if (status.state == FingerprintReaderState::Disabled) fail("Fingerprint hardware disabled");
    else if (status.state == FingerprintReaderState::RecoveryRequired) fail("Fingerprint recovery required");
    // Other errors are reported by the worker with the operation generation.
}

bool cameraState() {
    return currentState==AUTH_FACE || currentState==ENROLL_FACE || currentState==REENROLL_FACE;
}
void processCameraEvent(const CameraEvent &event) {
    if (expireWorkflowIfNeeded()) { discardCameraImage(event.image); return; }
    const CameraOperation expected=currentState==AUTH_FACE?CameraOperation::Authentication:CameraOperation::Enrollment;
    if(!cameraState() || !cameraCaptureRequested || !cameraCommandAccepted ||
       !event.generation || event.generation!=cameraGeneration || event.operation!=expected) {
        discardCameraImage(event.image); return;
    }
    if(event.error!=CameraError::None || !event.image) {
        discardCameraImage(event.image);
        fail(event.error!=CameraError::None?cameraErrorText(event.error):"Missing camera image"); return;
    }
    if(millis()-stateStartedMs>=CAMERA_POSITION_DELAY_MS+CAMERA_CAPTURE_TIMEOUT_MS+2000) {
        discardCameraImage(event.image); fail("Camera step timed out"); return;
    }
    BackendRequest job{};
    job.operation=currentState==AUTH_FACE?BackendOperation::FaceAuth:BackendOperation::EnrollFace;
    job.image=event.image; job.cameraGeneration=event.generation;
    job.captureIndex=currentState==AUTH_FACE?0:acknowledgedFaceCount+1;
    if(!copyBackendText(job.flow,session.flow) ||
       !copyBackendText(job.credentialType,session.credentialType) ||
       !copyBackendText(job.authSessionId,session.authSessionId) ||
       !copyBackendText(job.enrollmentSessionId,session.enrollmentSessionId) ||
       (currentState==AUTH_FACE ? !job.authSessionId[0] : !job.enrollmentSessionId[0])) {
        discardCameraImage(event.image); fail("Missing face request context"); return;
    }
    queueBackendRequest(job); // Queue owns ready image; activeJob is a non-owning alias.
}
void checkCameraStatus() {
    if (expireWorkflowIfNeeded()) return;
    const CameraStatus status=getCameraStatus();
    static int lastLoggedState=-1;
    if(lastLoggedState!=static_cast<int>(status.state)) {
        lastLoggedState=static_cast<int>(status.state);
        switch(status.state) {
        case CameraState::Disabled: Serial.println("[Camera] Hardware disabled in HardwareConfig.h."); break;
        case CameraState::Idle: Serial.println("[Camera] Waiting; initializes only for a capture."); break;
        case CameraState::Starting: Serial.println("[Camera] Initializing driver."); break;
        case CameraState::Ready: Serial.println("[Camera] Driver initialized (not a face check)."); break;
        default: Serial.println("[Camera] Driver unavailable."); break;
        }
    }
    if(!cameraState())return;
    if(!cameraCommandAccepted){fail("Camera worker unavailable");return;}
    if(status.state==CameraState::Disabled){fail("Camera hardware disabled");return;}
    if(cameraCaptureRequested)return;
    const uint32_t elapsed=millis()-stateStartedMs;
    if(elapsed<CAMERA_POSITION_DELAY_MS) {
        const int seconds=static_cast<int>((CAMERA_POSITION_DELAY_MS-elapsed+999)/1000);
        if(seconds!=lastCameraCountdown) {
            lastCameraCountdown=seconds;
            char text[64];
            if(currentState==AUTH_FACE) {
                snprintf(text,sizeof(text),"n0.val=%d",seconds);sendNextion(text);
            } else {
                snprintf(text,sizeof(text),"%s (%d)",facePromptText[acknowledgedFaceCount],seconds);
                setText("t1",text);
            }
        }
        return;
    }
    if(++cameraGeneration==0)++cameraGeneration;
    CameraCommand command{}; command.generation=cameraGeneration;
    command.operation=currentState==AUTH_FACE?CameraOperation::Authentication:CameraOperation::Enrollment;
    cameraCommandAccepted=requestCameraCapture(command); cameraCaptureRequested=true;
    if(!cameraCommandAccepted){fail("Camera worker busy");return;}
    if(currentState==AUTH_FACE)sendNextion("n0.val=0");
    setText(currentState==AUTH_FACE?"t0":"t1","Capturing...");
}

void checkRFIDStatus() {
    const RfidStatus status = getRFIDStatus();
    static int lastLoggedState = -1;
    const int stateNumber = static_cast<int>(status.state);
    if (stateNumber != lastLoggedState) {
        lastLoggedState = stateNumber;
        switch (status.state) {
            case RfidReaderState::Disabled:
                Serial.println("[RFID] Hardware disabled in HardwareConfig.h."); break;
            case RfidReaderState::Starting:
                Serial.println("[RFID] Starting reader worker."); break;
            case RfidReaderState::Ready:
                Serial.println("[RFID] Reader register check passed."); break;
            default:
                Serial.println("[RFID] Reader unavailable; ID/PIN entry remains usable.");
                break;
        }
    }
    if (currentState == ENROLL_RFID || currentState == REENROLL_RFID) {
        if (!rfidCommandAccepted) fail("RFID worker unavailable");
        else if (status.state == RfidReaderState::Disabled) fail("RFID hardware disabled");
        else if (status.state == RfidReaderState::Unavailable) fail("RFID reader unavailable");
    }
}

void startIdEntry() {
    resetSession();
    changeState(ENTER_ID_PIN);
    showIdPinScreen();
}

void handleLine(char *line) {
    if (equalText(line, "READY")) {
        // Emit only in Program.s, NOT in any page's initialization event.
        displayedPage = "";
        sendNextion("bkcmd=0");
        returnToIdle();
        return;
    }
    if (equalText(line, "CANCEL") || equalText(line, "HOME")) {
        returnToIdle();
        return;
    }
    if (equalText(line, "START_ID_PIN")) {
        if (currentState == IDLE) startIdEntry();
        return;
    }
    if (equalText(line, "BACK_ID")) {
        if (currentState == ENTER_PIN)
            startIdEntry();
        return;
    }
    if (strncmp(line, "CHECK:", 6) == 0) {
        if (currentState != ENTER_ID_PIN) return;
        const char *id = line + 6;
        if (!exactDigits(id, ID_DIGITS)) {
            setText(ID_STATUS, "Enter a 5-digit ID");
            return;
        }
        if (!copyBackendText(currentEmployeeId, id)) {
            fail("Invalid ID");
            return;
        }
        // No ID-only endpoint and no backend request here. The ID check above
        // validates format only. The backend checks ID + PIN together later.
        changeState(ENTER_PIN);
        showPinScreen();
        return;
    }
    if (strncmp(line, "AUTH:", 5) == 0) {
        if (currentState != ENTER_PIN) return;
        char *id = line + 5;
        char *comma = strchr(id, ',');
        if (comma == nullptr || strchr(comma + 1, ',') != nullptr) {
            fail("Invalid ID/PIN message");
            return;
        }
        *comma = '\0';
        const char *pin = comma + 1;
        if (!exactDigits(id, ID_DIGITS) || !equalText(id, currentEmployeeId) ||
            !exactDigits(pin, PIN_DIGITS)) {
            fail("Invalid ID/PIN input");
            return;
        }
        submitIdPinRequest(pin);
        return;
    }
    // Legacy page8 VERIFY_PIN events intentionally have no action. This build
    // never opens page8; leave that unused page in the HMI rather than delete it.
}

struct LineParser {
    enum Mode { WAIT_START, READING, DISCARD } mode = WAIT_START;
    char line[MAX_LINE_LENGTH + 1] = {};
    size_t length = 0;
    uint32_t lastByteMs = 0;
    bool sawCR = false;

    void reset() {
        memset(line, 0, sizeof(line));
        length = 0;
        sawCR = false;
        mode = WAIT_START;
    }
    void checkTimeout() {
        if (mode != WAIT_START && millis() - lastByteMs >= PARTIAL_LINE_TIMEOUT_MS)
            reset();
    }
    void receive(uint8_t byte) {
        lastByteMs = millis();
        if (mode == WAIT_START) {
            if (byte == '@') mode = READING;
            return;
        }
        if (byte == '\n') {
            if (mode == READING && length > 0) {
                line[length] = '\0';
                handleLine(line);
            }
            reset();
            return;
        }
        if (mode == DISCARD) return;  // Never accept the tail of an overflowed line.
        if (byte == '\r' && !sawCR) { sawCR = true; return; }
        if (sawCR || byte < 32 || byte > 126 || byte == '@' ||
            length >= MAX_LINE_LENGTH) {
            mode = DISCARD;
            return;
        }
        line[length++] = static_cast<char>(byte);
    }
};

void checkLockProgress() {
    if (currentState != UNLOCK_PENDING && currentState != ACCESS_GRANTED) return;
    const LockStatus status = getLockStatus();
    if (status.pulseNumber != expectedLockPulse || status.state == LockState::Fault ||
        status.state == LockState::Disabled || status.state == LockState::NotInitialized) {
        fail("Lock output unavailable"); return;
    }
    if (currentState == UNLOCK_PENDING) {
        if (status.state == LockState::UnlockOutput && status.pulseStarted) {
            changeState(ACCESS_GRANTED);
            showSuccessScreen(); // GPIO command succeeded; NOT measured door position.
        } else if (status.state != LockState::PendingUnlock) {
            fail("Unlock interval ended");
        }
    }
}

void checkStateTimeout() {
    if (expireWorkflowIfNeeded()) return;
    const uint32_t elapsed = millis() - stateStartedMs;
    switch (currentState) {
        case IDLE: break;
        case ENTER_ID_PIN:
        case ENTER_PIN:
            if (elapsed >= ENTRY_TIMEOUT_MS) returnToIdle();
            break;
        case WAITING_FOR_BACKEND:
            if (elapsed >= BACKEND_TIMEOUT_MS) fail("Backend timed out");
            break;
        case AUTH_FACE:
        case ENROLL_FACE:
        case REENROLL_FACE:
            if(elapsed>=CAMERA_POSITION_DELAY_MS+CAMERA_CAPTURE_TIMEOUT_MS+2000)fail("Camera step timed out");
            break;
        case AUTH_FINGERPRINT:
            if (elapsed >= FINGER_MATCH_TIMEOUT_MS + 3000) fail("Fingerprint step timed out");
            break;
        case ENROLL_FINGERPRINT:
        case REENROLL_FINGERPRINT:
            if (elapsed >= FINGER_ENROLL_TIMEOUT_MS + 3000) fail("Fingerprint step timed out");
            break;
        case DELETE_OLD_FINGERPRINTS:
            if (elapsed >= FINGER_DELETE_TIMEOUT_MS + 3000) fail("Fingerprint deletion timed out");
            break;
        case UNLOCK_PENDING:
            // Also cancels an unserved pending pulse if the worker cannot run.
            if (elapsed >= LOCK_START_TTL_MS + 100) fail("Lock did not start");
            break;
        case ACCESS_GRANTED: {
            const LockStatus lock = getLockStatus();
            const bool outputRestored = lock.state == LockState::Cooldown ||
                                        lock.state == LockState::LockedOutput;
            if (elapsed >= SUCCESS_HOLD_MS && outputRestored) returnToIdle();
            else if (elapsed >= LOCK_MAX_PULSE_MS + LOCK_START_TTL_MS + 1000)
                fail("Lock output deadline exceeded");
            break;
        }
        case ACCESS_DENIED: {
            const size_t count = failurePages.count ? failurePages.count : 1;
            if (elapsed >= FAILURE_HOLD_MS * count) {
                if (startIdAfterFailure) startIdEntry();
                else returnToIdle();
            } else {
                const size_t page = elapsed / FAILURE_HOLD_MS;
                if (page != failurePageIndex) renderFailurePage(page);
            }
            break;
        }
        case ENROLLMENT_COMPLETE:
            if (elapsed >= ENROLLMENT_HOLD_MS) returnToIdle();
            break;
        default:
            if (elapsed >= STEP_TIMEOUT_MS) fail("Step timed out");
            break;
    }
}

void vUARTTask(void *) {
    // One task owns BOTH serial directions and the workflow: no display-write race.
    vTaskDelay(pdMS_TO_TICKS(1000));  // Startup grace only, not a UI blocking delay.
    sendNextion("bkcmd=0");
    returnToIdle();
    LineParser parser;
    BackendResponse response{};
    while (true) {
        checkStateTimeout();
        checkLockProgress();
        checkRFIDStatus();
        checkFingerprintStatus();
        checkCameraStatus();
        parser.checkTimeout();
        // Bound work per pass so a serial flood cannot starve response/time checks.
        for (size_t i = 0; i < 128 && nextionSerial.available() > 0; ++i) {
            const int value = nextionSerial.read();
            if (value >= 0) parser.receive(static_cast<uint8_t>(value));
        }
        for (size_t i = 0; i < 4 &&
             xQueueReceive(responseQueue, &response, 0) == pdTRUE; ++i) {
            applyBackendResponse(response);
            response = BackendResponse{};
        }
        RfidEvent event{};
        for (size_t i = 0; i < 4 && pollRFIDEvent(event); ++i) {
            processRFIDEvent(event);
            event = RfidEvent{};
        }
        FingerprintEvent fpEvent{};
        for (size_t i=0; i<8 && pollFingerprintEvent(fpEvent); ++i) {
            processFingerprintEvent(fpEvent); fpEvent=FingerprintEvent{};
        }
        CameraEvent cameraEvent{};
        for(size_t i=0;i<4 && pollCameraEvent(cameraEvent);++i) {
            processCameraEvent(cameraEvent); cameraEvent=CameraEvent{};
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void dispatchBackendRequest(const BackendRequest &request) {
    if (!isBackendRequestCurrent(request.requestId)) {
        discardCameraImage(request.image);
        return;
    }
    executingContext = BackendRequestContext{};
    executingContext.requestId = request.requestId;
    copyBackendText(executingContext.flow, request.flow);
    copyBackendText(executingContext.credentialType, request.credentialType);
    copyBackendText(executingContext.authSessionId, request.authSessionId);
    copyBackendText(executingContext.enrollmentSessionId, request.enrollmentSessionId);

    // Runs only in backend worker task, never the UI task or RFID task.
    // Preserve request.flow/credentialType/enrollmentSessionId here when adding
    // the future HTTP normalizer. Never borrow a newer live session from the UI.
    switch (request.operation) {
        case BackendOperation::IdPin:
            sendIdPinToBackend(String(request.employeeId), String(request.pin),
                               request.requestId);
            break;
        case BackendOperation::RfidAuth:
            sendRfidAuthToBackend(String(request.rfidUid), request.requestId);
            break;
        case BackendOperation::EnrollRfid:
            sendEnrollmentRfid(String(request.enrollmentSessionId),
                               String(request.rfidUid), request.requestId);
            break;
        case BackendOperation::FaceAuth:
        case BackendOperation::EnrollFace: {
            // Image lifetime is the synchronous hook invocation, not response lifetime.
            CameraImageView view{};
            if(!borrowCameraImage(request.image,view)) {
                BackendResponse error{}; error.requestId=request.requestId;
                copyBackendText(error.failureReason,"Camera image expired or cancelled");
                handleBackendResponse(error); break;
            }
            if(!view.faceDetected || !view.cropped) {
                BackendResponse error{};error.requestId=request.requestId;
                copyBackendText(error.failureReason,"Local face processing required");
                handleBackendResponse(error);releaseCameraImage(request.image);break;
            }
            backendFaceView=view; backendFaceRequestId=request.requestId;
            if(request.operation==BackendOperation::FaceAuth)
                sendFaceAuthToBackend(String(request.authSessionId),request.requestId);
            else sendEnrollmentFace(String(request.enrollmentSessionId),request.requestId);
            backendFaceRequestId=0; backendFaceView=CameraImageView{};
            releaseCameraImage(request.image); // Success, rejection, or transport failure.
            break;
        }
        case BackendOperation::FingerprintAuth:
            sendFingerprintAuthToBackend(String(request.authSessionId),request.templateSlot,request.requestId);
            break;
        case BackendOperation::EnrollFingerprint:
            sendEnrollmentFingerprint(String(request.enrollmentSessionId),request.templateSlot,request.requestId);
            break;
        case BackendOperation::ConfirmOldDeleted:
            confirmOldFingerprintsDeleted(String(request.enrollmentSessionId),request.requestId);
            break;
        default: {
            BackendResponse error{};
            error.requestId = request.requestId;
            copyBackendText(error.failureReason, "Unsupported backend operation");
            handleBackendResponse(error);
            break;
        }
    }
}

void vBackendTask(void *) {
    BackendRequest request{};
    while (true) {
        serviceBackendNetwork(); // Starts/reconnects Wi-Fi; no automatic API POST.
        if (xQueueReceive(requestQueue, &request, pdMS_TO_TICKS(100)) == pdTRUE) {
            dispatchBackendRequest(request);
            executingContext = BackendRequestContext{};
            memset(&request, 0, sizeof(request));
        }
    }
}
} // namespace

bool getBackendRequestContext(uint32_t requestId, BackendRequestContext &context) {
    context = BackendRequestContext{};
    if (!backendTaskHandle || xTaskGetCurrentTaskHandle() != backendTaskHandle ||
        !requestId || requestId != executingContext.requestId) return false;
    context = executingContext;
    return true;
}

bool isBackendRequestCurrent(uint32_t requestId) {
    RequestLease lease{};
    return requestId && requestLeaseQueue &&
        xQueuePeek(requestLeaseQueue, &lease, 0) == pdTRUE &&
        lease.requestId == requestId &&
        millis() - lease.startedMs < NetworkConfig::REQUEST_BUDGET_MS &&
        (!lease.workflowLifetimeMs || !BackendPolicy::elapsedAtLeast(
            millis(), lease.workflowStartedMs, lease.workflowLifetimeMs));
}

bool getBackendFaceImage(uint32_t requestId, CameraImageView &image) {
    image=CameraImageView{};
    if(!backendTaskHandle || xTaskGetCurrentTaskHandle()!=backendTaskHandle)return false;
    if(!requestId || requestId!=backendFaceRequestId || !backendFaceView.data)return false;
    image=backendFaceView; return true;
}

bool handleBackendResponse(const BackendResponse &response) {
    return responseQueue != nullptr &&
           xQueueSend(responseQueue, &response, pdMS_TO_TICKS(10)) == pdTRUE;
}

bool beginScreenController() {
    if (requestQueue != nullptr || responseQueue != nullptr) return false;
    nextionSerial.setRxBufferSize(512);
    nextionSerial.begin(NEXTION_BAUD, SERIAL_8N1,
                       NEXTION_RX_PIN, NEXTION_TX_PIN);
    if (!nextionSerial) return false;
    requestQueue = xQueueCreate(2, sizeof(BackendRequest));
    responseQueue = xQueueCreate(4, sizeof(BackendResponse));
    requestLeaseQueue = xQueueCreate(1, sizeof(RequestLease));
    if (requestLeaseQueue) {
        const RequestLease none{0, millis(), 0, 0};
        xQueueOverwrite(requestLeaseQueue, &none);
    }
    if (requestQueue != nullptr && responseQueue != nullptr && requestLeaseQueue != nullptr &&
        xTaskCreate(vBackendTask, "Backend", 16384, nullptr, 1,
                    &backendTaskHandle) == pdPASS &&
        xTaskCreate(vUARTTask, "NextionController", 6144, nullptr, 2,
                    &uiTaskHandle) == pdPASS) return true;

    if (backendTaskHandle != nullptr) {
        vTaskDelete(backendTaskHandle);
        backendTaskHandle = nullptr;
    }
    if (requestQueue != nullptr) vQueueDelete(requestQueue);
    if (responseQueue != nullptr) vQueueDelete(responseQueue);
    if (requestLeaseQueue != nullptr) vQueueDelete(requestLeaseQueue);
    requestLeaseQueue = nullptr;
    requestQueue = nullptr;
    responseQueue = nullptr;
    nextionSerial.end();
    return false;
}

// PlatformIO Arduino entry points. Do not add another setup()/loop() file.
void setup() {
    Serial.begin(115200);
    if (!initLock(LOCK_PIN))
        Serial.println("Lock startup failed. No unlock requests will be accepted.");
    if (!beginRFID())
        Serial.println("RFID worker startup failed. Required RFID steps will fail closed.");
    if (!beginFingerprint())
        Serial.println("Fingerprint worker startup failed. Required fingerprint steps will fail closed.");
    if (!beginCamera())
        Serial.println("Camera worker startup failed. Required camera steps will fail closed.");
    if (!beginScreenController()) {
        Serial.println("Screen startup failed: UART/queue/task initialization.");
        if (!lockNow()) Serial.println("Locked GPIO command failed.");
        while (true) delay(1000);
    }
    Serial.println("v8.2: HTTPS + fixed workflow deadlines + slots 1-162. Hardware flags unchanged.");
}

void loop() {
    delay(1000); // Controller, backend, sensor, and lock tasks do the work.
}
