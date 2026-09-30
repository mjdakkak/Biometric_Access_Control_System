#pragma once
#include <stddef.h>
#include <stdint.h>
#include <type_traits>
#include "face_processing.h"

// Captures and crops one face; identity checks belong to the backend.
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
    CameraImageHandle image = 0; // Receiver owns a successful image handle.
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
    uint64_t capturedAtUs = 0; // Microseconds since boot.
    bool faceDetected = false;
    bool cropped = false;
    uint16_t sourceWidth = 0, sourceHeight = 0;
    FaceRectangle face{}, crop{}; // Coordinates in the original frame.
    float detectionScore = 0;
};
static_assert(std::is_trivially_copyable<CameraCommand>::value, "Queue payload");
static_assert(std::is_trivially_copyable<CameraEvent>::value, "Queue payload");

// Start the worker; the driver initializes on its first capture.
bool beginCamera();
// A new generation cancels old work; it cannot interrupt a driver call.
bool requestCameraCapture(const CameraCommand &command);
bool pollCameraEvent(CameraEvent &event);
CameraStatus getCameraStatus();
const char *cameraErrorText(CameraError error);

// Borrow once, read synchronously, then release. Discard frees only unborrowed images.
bool borrowCameraImage(CameraImageHandle image, CameraImageView &view);
bool discardCameraImage(CameraImageHandle image);
bool releaseCameraImage(CameraImageHandle image);
