#pragma once

#include <Arduino.h>
#include <string.h>
#include <type_traits>
#include "camera_module.h"

// Transport limits, NOT the server's password policy. Agree these with your pair.
constexpr size_t MAX_EMPLOYEE_ID_LENGTH = 32;
constexpr size_t MAX_PIN_LENGTH = 16;
constexpr size_t MAX_SESSION_ID_LENGTH = 96;
constexpr size_t MAX_OLD_FINGERPRINT_SLOTS = 2;

// INTERNAL queue response, not the backend's literal JSON schema.
// Initialize every response with: BackendResponse response{};
// These are fixed arrays because FreeRTOS queues copy bytes, not String ownership.
struct BackendResponse {
    uint32_t requestId = 0;  // Copy from the request that produced this response.
    bool success = false;
    char flow[20] = "";                // AUTHENTICATION / ENROLLMENT / REENROLLMENT
    char nextStep[32] = "";             // RFID / FACE / FINGERPRINT /
                                       // DELETE_OLD_FINGERPRINTS / COMPLETE
    char credentialType[16] = "";      // RFID / FACE / FINGERPRINT, or empty
    char authSessionId[MAX_SESSION_ID_LENGTH + 1] = "";
    char enrollmentSessionId[MAX_SESSION_ID_LENGTH + 1] = "";
    char failureReason[81] = "";       // User-safe message, NOT a raw server trace.
    int fingerprintSlot = -1;
    // Optional normalized progress: -1 means omitted. Sensor/controller still
    // enforce the two-template workflow from locally acknowledged operations.
    int fingerprintsEnrolled = -1;
    int fingerprintsRequired = -1;
    // Camera enrollment progress. -1 = omitted; never inherit an old nextPrompt.
    int capturesCompleted = -1;
    int capturesRequired = -1;
    int nextCaptureIndex = -1;
    char nextPrompt[32] = "";
    bool hasOldFingerprintSlots = false;  // Missing list != explicitly empty list.
    uint8_t oldFingerprintSlotCount = 0;
    int oldFingerprintSlots[MAX_OLD_FINGERPRINT_SLOTS] = {-1, -1};
};

static_assert(std::is_trivially_copyable<BackendResponse>::value,
              "Queue payload must remain trivially copyable");

// No silent truncation. Source must be a valid C string. Check the return value
// when copying server data; reject oversize values instead of using partial IDs.
template <size_t N>
bool copyBackendText(char (&destination)[N], const char *source) {
    destination[0] = '\0';
    if (source == nullptr) return false;
    size_t length = 0;
    while (length < N && source[length] != '\0') ++length;
    if (length == N) return false;
    memcpy(destination, source, length + 1);
    return true;
}

// Network -> UI. Task context only, after beginScreenController() succeeds.
// Returns true if QUEUED, not if approved. Do not modify a response while calling.
// A false return must be handled by the caller; the UI otherwise times out.
bool handleBackendResponse(const BackendResponse &response);

// Single entry endpoint: POST /kiosk/id-pin with employee_id + pin.
// The ID page only stores/validates format locally. No ID-only HTTP request.
// The same pin field covers AUTHENTICATION, ENROLLMENT, and REENROLLMENT.
// sendEmployeeIdToBackend/sendSetupPinToBackend have been removed: those extra
// hooks belonged to the earlier, unconfirmed two-request design.
//
// ORIGINAL EIGHT function names and requestId-last signatures are preserved.
// v8 dispatches all eight hooks through the HTTPS client in BackendStubs.cpp.
// Face hooks obtain their immutable JPEG through getBackendFaceImage below.
// For sendFingerprintAuthToBackend ONLY, matchedTemplateSlot == -1 means a
// completed search found no match. The future HTTP client MUST serialize that
// as matched_template_slot: null. A communication error is not a no-match.
//
// HTTP ADAPTER CONTRACT:
// - Save immutable context with EACH request (requestId, operation, flow,
//   credential type and the relevant session ID). Do not read live UI globals.
// - The first /kiosk/id-pin success must establish flow + session + next_step.
// - For follow-ups, missing flow/session/credential fields inherit only from
//   that original request context. Missing is not the same as null/empty.
//   Reject malformed values; reject contradictory workflow/session identities.
// - Clear operation-specific slots/deletion lists for each new response; require
//   an explicit new fingerprint_slot or old_slots for the step that needs it.
// - Map authentication current_state=COMPLETE to nextStep=COMPLETE only for an
//   authentication completion response; reject conflicting state fields.
// - Map JSON into a complete validated BackendResponse BEFORE queueing it.
// - Keep original requestId association; ignore cancelled/late results in UI.
// - A true success on an intermediate operation is NOT access authorization.
// Implemented by the v8 client. Recovery maintenance endpoints remain separate.
void sendIdPinToBackend(String employeeId, String pin, uint32_t requestId);
void sendRfidAuthToBackend(String rfidUid, uint32_t requestId);
void sendFaceAuthToBackend(String sessionId, uint32_t requestId);
void sendFingerprintAuthToBackend(String sessionId, int matchedTemplateSlot,
                                  uint32_t requestId);
void sendEnrollmentRfid(String sessionId, String rfidUid, uint32_t requestId);
void sendEnrollmentFace(String sessionId, uint32_t requestId);
void sendEnrollmentFingerprint(String sessionId, int templateSlot,
                               uint32_t requestId);
void confirmOldFingerprintsDeleted(String sessionId, uint32_t requestId);

// FACE IMAGE BRIDGE: call ONLY inside sendFaceAuthToBackend/sendEnrollmentFace,
// on the backend worker task, using that hook's requestId. View is borrowed until
// the hook returns. Do not free it, keep it, or start an asynchronous upload using
// it. Consume bytes synchronously (with a network deadline); dispatcher releases
// the image even when the HTTP request fails. Wrong request/task returns false.
// v7 view is a locally detected, padded face crop: faceDetected=true, cropped=true.
// Crop dimensions vary. The backend still must verify identity/authorization;
// detection alone does not prove liveness, identity, pose, or image quality.
// HTTP fields: request_id, relevant session ID, image (image/jpeg).
bool getBackendFaceImage(uint32_t requestId, CameraImageView &image);

// v8 immutable request context bridge. Only the backend worker can retrieve it;
// it never contains a PIN or device key. No networking task reads live UI data.
struct BackendRequestContext {
    uint32_t requestId = 0;
    char flow[20] = "";
    char credentialType[16] = "";
    char authSessionId[MAX_SESSION_ID_LENGTH + 1] = "";
    char enrollmentSessionId[MAX_SESSION_ID_LENGTH + 1] = "";
};
bool getBackendRequestContext(uint32_t requestId, BackendRequestContext &context);
// Uses a queue snapshot, not unsynchronized controller globals.
bool isBackendRequestCurrent(uint32_t requestId);
// Call only from the single existing backend worker. Never called by the UI.
void serviceBackendNetwork();
