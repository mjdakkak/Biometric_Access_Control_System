#pragma once
#include <stddef.h>
#include <stdint.h>

// Single-face detection and cropping; no identity or liveness check.
// Called by the camera worker only.
enum class FaceProcessingError : uint8_t {
    None, Disabled, InvalidJpeg, NoMemory, DecodeFailed, NoFace, MultipleFaces,
    InvalidDetection, FaceTooSmall, FaceAtEdge, EncodeFailed, Cancelled
};
struct FaceRectangle {
    uint16_t x=0, y=0, width=0, height=0; // Half-open rectangle in original-image pixels.
};
struct ProcessedFaceImage {
    uint8_t *data=nullptr; // Release with freeProcessedFaceImage().
    size_t length=0;
    size_t allocatedBytes=0;
    uint16_t width=0, height=0, sourceWidth=0, sourceHeight=0;
    FaceRectangle face{}, crop{};
    float detectionScore=0;
    bool faceDetected=false, cropped=false;
};
using FaceStopRequested = bool (*)(void *context);

FaceProcessingError detectAndCropFace(const uint8_t *jpeg, size_t length,
                                     uint16_t width, uint16_t height,
                                     ProcessedFaceImage &output,
                                     FaceStopRequested stop=nullptr, void *context=nullptr);
void freeProcessedFaceImage(ProcessedFaceImage &image);
const char *faceProcessingErrorText(FaceProcessingError error);

// Detector boxes include the right/bottom edge; returned rectangles use width/height.
bool jpegDimensionsMatch(const uint8_t *jpeg, size_t length, uint16_t width, uint16_t height);
FaceProcessingError faceRectangles(int left, int top, int right, int bottom,
                                  uint16_t detectionWidth, uint16_t detectionHeight,
                                  uint16_t sourceWidth, uint16_t sourceHeight,
                                  FaceRectangle &face, FaceRectangle &crop);
