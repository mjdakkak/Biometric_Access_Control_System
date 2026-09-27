#pragma once
#include <stddef.h>
#include <stdint.h>
#include <type_traits>
#include "face_processing.h"

// Capture + local single-face detection/cropping. No recognition or liveness.
// Handles reference bounded, owned JPEG copies, NOT driver camera_fb_t pointers.
using CameraImageHandle = uint64_t;
enum class CameraOperation : uint8_t { None, Authentication, Enrollment };
enum class CameraState : uint8_t { Disabled, Idle, Starting, Ready, Unavailable };
enum class CameraError : uint8_t {
    None, Disabled, MissingPsram, InitFailed, CaptureFailed, TimedOut,
    InvalidJpeg, StaleFrame, NoMemory, ImagePoolBusy, FaceProcessingDisabled,
    NoFace, MultipleFaces, FaceTooSmall, FaceAtEdge, FaceProcessingFailed
};
struct CameraCommand {
    CameraOperation operation = CameraOperation::None;
    uint32_t generation = 0;
};
struct CameraEvent {
    CameraOperation operation = CameraOperation::None;
    uint32_t generation = 0;
    CameraError error = CameraError::None;
    CameraImageHandle image = 0; // Nonzero only on success. Receiver owns disposal.
};
struct CameraStatus {
    CameraState state = CameraState::Disabled;
    CameraError error = CameraError::None;
    int32_t driverError = 0;
};
struct CameraImageView {
    const uint8_t *data = nullptr;
    size_t length = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint64_t capturedAtUs = 0; // Since boot, not a UTC timestamp.
    bool faceDetected = false;
    bool cropped = false;
    uint16_t sourceWidth = 0, sourceHeight = 0;
    FaceRectangle face{}, crop{}; // In original source-frame coordinates.
    float detectionScore = 0; // Not an identity/match confidence.
};
static_assert(std::is_trivially_copyable<CameraCommand>::value, "Queue payload");
static_assert(std::is_trivially_copyable<CameraEvent>::value, "Queue payload");

// Setup: creates queues/task/mutex. Does not prove hardware availability.
// Camera hardware initializes lazily on the first enabled capture request.
bool beginCamera();
// Single controller, task context only. A new nonzero generation per operation.
// Latest command wins. None cancels; an already-running driver call is not interrupted.
bool requestCameraCapture(const CameraCommand &command);
bool pollCameraEvent(CameraEvent &event);
CameraStatus getCameraStatus();
const char *cameraErrorText(CameraError error);

// Lease rules (all task context, never ISR):
// - Worker publishes one owned JPEG copy via a nonzero handle.
// - Controller transfers that handle to a backend queue, or discards it.
// - Backend borrows once, consumes bytes synchronously, then releases once.
// - discard only frees UNBORROWED images; cancelling cannot free an active upload.
// - view/pointers become invalid after release. Never retain them in another task.
// - Unborrowed images expire; borrowed images are not reclaimed underneath a reader.
bool borrowCameraImage(CameraImageHandle image, CameraImageView &view);
bool discardCameraImage(CameraImageHandle image);
bool releaseCameraImage(CameraImageHandle image);
