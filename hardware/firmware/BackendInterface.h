#pragma once

#include <Arduino.h>
#include <string.h>
#include <type_traits>
#include "camera_module.h"

// Client buffer limits.
constexpr size_t MAX_EMPLOYEE_ID_LENGTH = 32;
constexpr size_t MAX_PIN_LENGTH = 16;
constexpr size_t MAX_SESSION_ID_LENGTH = 96;
constexpr size_t MAX_OLD_FINGERPRINT_SLOTS = 2;

// Fixed-size fields keep queue messages safe to copy.
struct BackendResponse {
    uint32_t requestId = 0;  // ID of the request that produced this response.
    bool success = false;
    char flow[20] = "";
    char nextStep[32] = "";
    char credentialType[16] = "";
    char authSessionId[MAX_SESSION_ID_LENGTH + 1] = "";
    char enrollmentSessionId[MAX_SESSION_ID_LENGTH + 1] = "";
    char failureReason[81] = "";       // User-facing reason; no server traces.
    int fingerprintSlot = -1;
    // -1 means the optional progress value was omitted.
    int fingerprintsEnrolled = -1;
    int fingerprintsRequired = -1;
    int capturesCompleted = -1;
    int capturesRequired = -1;
    int nextCaptureIndex = -1;
    char nextPrompt[32] = "";
    bool hasOldFingerprintSlots = false;  // Distinguishes an omitted list from an empty one.
    uint8_t oldFingerprintSlotCount = 0;
    int oldFingerprintSlots[MAX_OLD_FINGERPRINT_SLOTS] = {-1, -1};
};

static_assert(std::is_trivially_copyable<BackendResponse>::value,
              "Queue payload must remain trivially copyable");

// Reject oversized values instead of truncating IDs.
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

// Queue a response for the controller; true means queued, not approved.
bool handleBackendResponse(const BackendResponse &response);

// ID and PIN are submitted together; the backend selects the workflow.
void sendIdPinToBackend(String employeeId, String pin, uint32_t requestId);
void sendRfidAuthToBackend(String rfidUid, uint32_t requestId);
void sendFaceAuthToBackend(String sessionId, uint32_t requestId);
// Fingerprint slot -1 means no match and is sent as JSON null.
void sendFingerprintAuthToBackend(String sessionId, int matchedTemplateSlot,
                                  uint32_t requestId);
void sendEnrollmentRfid(String sessionId, String rfidUid, uint32_t requestId);
void sendEnrollmentFace(String sessionId, uint32_t requestId);
void sendEnrollmentFingerprint(String sessionId, int templateSlot,
                               uint32_t requestId);
void confirmOldFingerprintsDeleted(String sessionId, uint32_t requestId);

// Borrow the JPEG only inside a face hook; the view expires when the hook returns.
bool getBackendFaceImage(uint32_t requestId, CameraImageView &image);

// Snapshot of the request owned by the backend worker.
struct BackendRequestContext {
    uint32_t requestId = 0;
    char flow[20] = "";
    char credentialType[16] = "";
    char authSessionId[MAX_SESSION_ID_LENGTH + 1] = "";
    char enrollmentSessionId[MAX_SESSION_ID_LENGTH + 1] = "";
};
bool getBackendRequestContext(uint32_t requestId, BackendRequestContext &context);
bool isBackendRequestCurrent(uint32_t requestId);
// Call only from the backend worker.
void serviceBackendNetwork();
