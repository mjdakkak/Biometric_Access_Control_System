#include "camera_module.h"
#include "HardwareConfig.h"
#include "face_processing.h"
#include <Arduino.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "camera_snapshot_preview.h"

#ifndef KIOSK_CAMERA_DIAGNOSTICS
#define KIOSK_CAMERA_DIAGNOSTICS 1
#endif
#include <stdio.h>
#include <stdarg.h>

namespace camera_detail {
void cameraDiagnostic(const char *format, ...) {
#if KIOSK_CAMERA_DIAGNOSTICS
    char text[224];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Serial.println(text);
#else
    (void)format;
#endif
}

using namespace HardwareConfig;
QueueHandle_t commandQueue = nullptr, eventQueue = nullptr, statusQueue = nullptr;
SemaphoreHandle_t imageMutex = nullptr;
TaskHandle_t workerTask = nullptr;

enum class ImageState : uint8_t { Free, Ready, Borrowed };
struct ImageSlot {
    ImageState state = ImageState::Free;
    CameraImageHandle handle = 0;
    uint8_t *data = nullptr;
    size_t length = 0;
    size_t allocatedBytes = 0;
    uint16_t width = 0, height = 0;
    uint64_t capturedAtUs = 0;
    uint32_t createdMs = 0;
    bool faceDetected=false, cropped=false;
    uint16_t sourceWidth=0, sourceHeight=0;
    FaceRectangle face{}, crop{};
    float detectionScore=0;
};
ImageSlot images[CAMERA_IMAGE_POOL_SIZE]; // Protected by imageMutex.
CameraImageHandle nextImage = 0;

struct ImageLock {
    bool locked;
    ImageLock() : locked(imageMutex && xSemaphoreTake(imageMutex, portMAX_DELAY) == pdTRUE) {}
    ~ImageLock() { if (locked) xSemaphoreGive(imageMutex); }
    ImageLock(const ImageLock &) = delete;
    ImageLock &operator=(const ImageLock &) = delete;
};
void clearSlot(ImageSlot &s) {
    // Wipe our copy before freeing it.
    if (s.data) {
        volatile uint8_t *p = s.data;
        for (size_t i=0; i<s.allocatedBytes; ++i) p[i] = 0;
        heap_caps_free(s.data);
    }
    s = ImageSlot{};
}
void expireReadyImages() {
    ImageLock lock;
    if (!lock.locked) return;
    for (auto &s : images)
        if (s.state == ImageState::Ready && millis()-s.createdMs >= CAMERA_IMAGE_TTL_MS)
            clearSlot(s);
}
// Transfer the processed JPEG into the image pool.
CameraImageHandle adoptFace(ProcessedFaceImage &face, uint64_t timestamp, CameraError &error) {
    ImageLock lock;
    if (!lock.locked) { error=CameraError::ImagePoolBusy; return 0; }
    if (!face.data || !face.faceDetected || !face.cropped || !face.length) {
        error=CameraError::FaceProcessingFailed; return 0;
    }
    for (auto &s:images) if(s.state==ImageState::Free) {
        if(++nextImage==0)++nextImage;
        s.handle=nextImage; s.data=face.data; s.length=face.length;
        s.allocatedBytes=face.allocatedBytes; s.width=face.width; s.height=face.height;
        s.sourceWidth=face.sourceWidth;s.sourceHeight=face.sourceHeight;
        s.face=face.face;s.crop=face.crop;s.detectionScore=face.detectionScore;
        s.faceDetected=true;s.cropped=true;s.capturedAtUs=timestamp;
        s.createdMs=millis();s.state=ImageState::Ready;
        face=ProcessedFaceImage{};error=CameraError::None;return s.handle;
    }
    error=CameraError::ImagePoolBusy;return 0;
}
CameraError mapFaceError(FaceProcessingError e) {
    switch(e){
    case FaceProcessingError::None:return CameraError::None;
    case FaceProcessingError::Disabled:return CameraError::FaceProcessingDisabled;
    case FaceProcessingError::NoFace:return CameraError::NoFace;
    case FaceProcessingError::MultipleFaces:return CameraError::MultipleFaces;
    case FaceProcessingError::FaceTooSmall:return CameraError::FaceTooSmall;
    case FaceProcessingError::FaceAtEdge:return CameraError::FaceAtEdge;
    case FaceProcessingError::NoMemory:return CameraError::NoMemory;
    case FaceProcessingError::InvalidJpeg:return CameraError::InvalidJpeg;
    default:return CameraError::FaceProcessingFailed;
    }
}

// Only the camera worker accesses the driver and capture state.
CameraCommand command{};
CameraStatus status{};
bool initialized=false, active=false, pending=false;
uint32_t startedMs=0;
uint64_t earliestFrameUs=0;
uint8_t warmupRemaining=0;
CameraEvent pendingEvent{};
void publish(CameraState state, CameraError error=CameraError::None, int32_t driverError=0) {
    status.state=state; status.error=error; status.driverError=driverError;
    xQueueOverwrite(statusQueue,&status);
}
void applyCommand() {
    CameraCommand next{};
    if (xQueueReceive(commandQueue,&next,0) != pdTRUE) return;
    if (next.generation == command.generation && next.operation == command.operation) return;
    // The receiver owns an image once its event is sent.
    if (pending && pendingEvent.image) discardCameraImage(pendingEvent.image);
    pending=false; pendingEvent=CameraEvent{};
    command=next; active=next.operation != CameraOperation::None;
    startedMs=millis(); earliestFrameUs=static_cast<uint64_t>(esp_timer_get_time());
    warmupRemaining=initialized ? 0 : CAMERA_WARMUP_FRAMES;
    if(active) cameraDiagnostic("[CAMDBG] capture requested t=%lu ms; generation=%lu; configured UI positioning delay=%lu ms",
        static_cast<unsigned long>(startedMs),static_cast<unsigned long>(command.generation),
        static_cast<unsigned long>(CAMERA_POSITION_DELAY_MS));
}
void finish(CameraError error, CameraImageHandle image=0) {
    cameraDiagnostic("[CAMDBG] finished after %lu ms: %s",
        static_cast<unsigned long>(millis()-startedMs),
        error==CameraError::None ? "Face crop ready" : cameraErrorText(error));
    pendingEvent=CameraEvent{}; pendingEvent.operation=command.operation;
    pendingEvent.generation=command.generation; pendingEvent.error=error;
    pendingEvent.image=image; pending=true; active=false;
}
bool deadlinePassed() { return millis()-startedMs >= CAMERA_CAPTURE_TIMEOUT_MS; }
camera_config_t makeConfig() {
    camera_config_t c = {};
    c.pin_pwdn=CAMERA_PWDN_PIN; c.pin_reset=CAMERA_RESET_PIN;
    c.pin_xclk=CAMERA_XCLK_PIN; c.pin_pclk=CAMERA_PCLK_PIN;
    c.pin_vsync=CAMERA_VSYNC_PIN; c.pin_href=CAMERA_HREF_PIN;
    c.pin_d0=CAMERA_D0_PIN; c.pin_d1=CAMERA_D1_PIN;
    c.pin_d2=CAMERA_D2_PIN; c.pin_d3=CAMERA_D3_PIN;
    c.pin_d4=CAMERA_D4_PIN; c.pin_d5=CAMERA_D5_PIN;
    c.pin_d6=CAMERA_D6_PIN; c.pin_d7=CAMERA_D7_PIN;
    // Use the SCCB fields from Arduino-ESP32 2.0.17.
    c.pin_sccb_sda = CAMERA_SDA_PIN;
    c.pin_sccb_scl = CAMERA_SCL_PIN;
    c.xclk_freq_hz=20000000;
    c.ledc_channel=LEDC_CHANNEL_0; c.ledc_timer=LEDC_TIMER_0;
    c.pixel_format=PIXFORMAT_JPEG;
    c.frame_size=psramFound() ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
    c.jpeg_quality=12; c.fb_count=1;
    c.fb_location=psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
    c.grab_mode=CAMERA_GRAB_WHEN_EMPTY;
    return c;
}
bool ensureCamera() {
    if (initialized) return true;

    if (CAMERA_REQUIRE_PSRAM && !psramFound()) {
        publish(CameraState::Unavailable, CameraError::MissingPsram);
        finish(CameraError::MissingPsram);
        return false;
    }

    publish(CameraState::Starting);
    camera_config_t config = makeConfig();
    const esp_err_t result = esp_camera_init(&config);
    if (result != ESP_OK) {
        cameraDiagnostic("[CAMDBG] esp_camera_init failed: 0x%X",
                         static_cast<unsigned>(result));
        publish(CameraState::Unavailable, CameraError::InitFailed, result);
        finish(CameraError::InitFailed);
        return false;
    }

    // Correct the OV3660 image orientation for this mounting.
    sensor_t *orientationSensor = esp_camera_sensor_get();
    if (orientationSensor == nullptr ||
        orientationSensor->set_vflip == nullptr ||
        orientationSensor->set_hmirror == nullptr) {
        cameraDiagnostic("[CAMDBG] Camera orientation controls unavailable.");
        const esp_err_t cleanup = esp_camera_deinit();
        if (cleanup != ESP_OK) {
            cameraDiagnostic("[CAMDBG] Camera cleanup failed: 0x%X",
                             static_cast<unsigned>(cleanup));
        }
        initialized = false;
        publish(CameraState::Unavailable, CameraError::InitFailed, ESP_FAIL);
        finish(CameraError::InitFailed);
        return false;
    }

    const int flipResult = orientationSensor->set_vflip(orientationSensor, 1);
    const int mirrorResult = orientationSensor->set_hmirror(orientationSensor, 0);
    cameraDiagnostic(
        "[CAMDBG] orientation: set_vflip(1)=%d; set_hmirror(0)=%d",
        flipResult, mirrorResult);

    if (flipResult != 0 || mirrorResult != 0) {
        // A failed orientation setting leaves the camera unavailable.
        const esp_err_t cleanup = esp_camera_deinit();
        if (cleanup != ESP_OK) {
            cameraDiagnostic("[CAMDBG] Camera cleanup failed: 0x%X",
                             static_cast<unsigned>(cleanup));
        }
        initialized = false;
        publish(CameraState::Unavailable, CameraError::InitFailed, ESP_FAIL);
        finish(CameraError::InitFailed);
        return false;
    }

    cameraDiagnostic("[CAMDBG] driver PID=0x%04X; cached vflip=%u hmirror=%u",
        static_cast<unsigned>(orientationSensor->id.PID),
        static_cast<unsigned>(orientationSensor->status.vflip),
        static_cast<unsigned>(orientationSensor->status.hmirror));

    initialized = true;
    publish(CameraState::Ready);
    return true;
}
bool sameOperation(const CameraCommand &saved) {
    return saved.generation==command.generation && saved.operation==command.operation && active;
}
uint64_t frameTimestamp(const camera_fb_t &f) {
    if (f.timestamp.tv_sec < 0 || f.timestamp.tv_usec < 0 || f.timestamp.tv_usec >= 1000000)
        return 0;
    return static_cast<uint64_t>(f.timestamp.tv_sec)*1000000ULL + f.timestamp.tv_usec;
}
bool plausibleJpeg(const camera_fb_t &f) {
    const size_t width=psramFound()?640:320, height=psramFound()?480:240;
    return f.buf && f.format==PIXFORMAT_JPEG && f.width==width && f.height==height &&
           f.len>=4 && f.len<=CAMERA_MAX_JPEG_BYTES && f.buf[0]==0xFF && f.buf[1]==0xD8 &&
           f.buf[f.len-2]==0xFF && f.buf[f.len-1]==0xD9;
}
bool stopFaceProcessing(void *saved) {
    applyCommand();
    return !sameOperation(*static_cast<CameraCommand*>(saved)) || deadlinePassed();
}
void tickWorker() {
    applyCommand(); expireReadyImages();
    camera_snapshot_preview::service(active || pending);
    if (pending) {
        if (xQueueSend(eventQueue,&pendingEvent,0)==pdTRUE) {
            pending=false; pendingEvent=CameraEvent{};
        }
        return;
    }
    if (!active) return;
    if (!CAMERA_ENABLED) { finish(CameraError::Disabled); return; }
    if (deadlinePassed()) { finish(CameraError::TimedOut); return; }
    const CameraCommand saved=command;
    if (!ensureCamera()) return;
    applyCommand();
    if (!sameOperation(saved)) return;
    if (deadlinePassed()) { finish(CameraError::TimedOut); return; }

    // Frame acquisition may block; keep it on this worker.
    camera_fb_t *frame=esp_camera_fb_get();
    applyCommand();
    if (!sameOperation(saved)) { if (frame) esp_camera_fb_return(frame); return; }
    if (deadlinePassed()) {
        if (frame) esp_camera_fb_return(frame);
        finish(CameraError::TimedOut); return;
    }
    if (!frame) {
        publish(CameraState::Unavailable,CameraError::CaptureFailed);
        finish(CameraError::CaptureFailed);
        esp_camera_deinit(); initialized=false; return;
    }
    if (warmupRemaining) {
        cameraDiagnostic("[CAMDBG] discarding warmup frame; remaining before discard=%u",
            static_cast<unsigned>(warmupRemaining));
        --warmupRemaining; esp_camera_fb_return(frame);
        earliestFrameUs=static_cast<uint64_t>(esp_timer_get_time()); return;
    }
    const uint64_t timestamp=frameTimestamp(*frame);
    const uint64_t now=static_cast<uint64_t>(esp_timer_get_time());
    if (!timestamp || timestamp>now) {
        esp_camera_fb_return(frame); finish(CameraError::StaleFrame); return;
    }
    if (timestamp<earliestFrameUs || now-timestamp>CAMERA_MAX_FRAME_AGE_MS*1000ULL) {
        // Skip frames buffered before the current request.
        esp_camera_fb_return(frame); return;
    }
    if (!plausibleJpeg(*frame)) {
        esp_camera_fb_return(frame); finish(CameraError::InvalidJpeg); return;
    }
    cameraDiagnostic("[CAMDBG] fresh frame=%lux%lu, JPEG=%lu bytes, age=%lu ms; entering local detector",
        static_cast<unsigned long>(frame->width),static_cast<unsigned long>(frame->height),
        static_cast<unsigned long>(frame->len),static_cast<unsigned long>((now-timestamp)/1000ULL));
    // Preview the same frame passed to the detector.
    camera_snapshot_preview::publish(*frame,saved.generation);
    applyCommand();
    if (!sameOperation(saved)) { esp_camera_fb_return(frame); return; }
    if (deadlinePassed()) { esp_camera_fb_return(frame); finish(CameraError::TimedOut); return; }
    // Keep the driver frame until processing finishes.
    ProcessedFaceImage processed{};
    CameraCommand context=saved;
    const FaceProcessingError faceError=detectAndCropFace(frame->buf,frame->len,
        static_cast<uint16_t>(frame->width),static_cast<uint16_t>(frame->height),
        processed,stopFaceProcessing,&context);
    esp_camera_fb_return(frame);
    applyCommand();
    if(!sameOperation(saved)){freeProcessedFaceImage(processed);return;}
    if(deadlinePassed()){freeProcessedFaceImage(processed);finish(CameraError::TimedOut);return;}
    CameraError error=mapFaceError(faceError);
    const CameraImageHandle image=faceError==FaceProcessingError::None ? adoptFace(processed,timestamp,error) : 0;
    freeProcessedFaceImage(processed); // Safe after ownership has moved to the pool.
    applyCommand();
    if (!sameOperation(saved)) { if(image)discardCameraImage(image); return; }
    if (deadlinePassed()) { if(image)discardCameraImage(image); finish(CameraError::TimedOut); return; }
    finish(error,image);
}
void worker(void *) {
    for (;;) { tickWorker(); vTaskDelay(pdMS_TO_TICKS(CAMERA_POLL_MS)); }
}
}

bool beginCamera() {
    using namespace camera_detail;
    if (commandQueue || eventQueue || statusQueue || imageMutex) return false;
    commandQueue=xQueueCreate(1,sizeof(CameraCommand));
    eventQueue=xQueueCreate(4,sizeof(CameraEvent));
    statusQueue=xQueueCreate(1,sizeof(CameraStatus));
    imageMutex=xSemaphoreCreateMutex();
    if (commandQueue && eventQueue && statusQueue && imageMutex) {
        publish(HardwareConfig::CAMERA_ENABLED ? CameraState::Idle : CameraState::Disabled);
        if (xTaskCreate(worker,"Camera",12288,nullptr,1,&workerTask)==pdPASS) return true;
    }
    if(commandQueue)vQueueDelete(commandQueue);
    if(eventQueue)vQueueDelete(eventQueue);
    if(statusQueue)vQueueDelete(statusQueue);
    if(imageMutex)vSemaphoreDelete(imageMutex);
    commandQueue=eventQueue=statusQueue=nullptr; imageMutex=nullptr; return false;
}
bool requestCameraCapture(const CameraCommand &c) {
    using namespace camera_detail;
    if (!commandQueue || !c.generation ||
        (c.operation!=CameraOperation::None && c.operation!=CameraOperation::Authentication &&
         c.operation!=CameraOperation::Enrollment)) return false;
    return xQueueOverwrite(commandQueue,&c)==pdTRUE;
}
bool pollCameraEvent(CameraEvent &e) {
    return camera_detail::eventQueue && xQueueReceive(camera_detail::eventQueue,&e,0)==pdTRUE;
}
CameraStatus getCameraStatus() {
    CameraStatus s{};
    s.state=CameraState::Unavailable;
    if(camera_detail::statusQueue)xQueuePeek(camera_detail::statusQueue,&s,0);
    return s;
}
bool borrowCameraImage(CameraImageHandle image,CameraImageView &view) {
    using namespace camera_detail;
    view=CameraImageView{}; ImageLock lock;
    if(!image || !lock.locked) return false;
    for(auto &s:images) if(s.handle==image && s.state==ImageState::Ready) {
        if(millis()-s.createdMs>=HardwareConfig::CAMERA_IMAGE_TTL_MS){clearSlot(s);return false;}
        s.state=ImageState::Borrowed; view.data=s.data;view.length=s.length;
        view.width=s.width;view.height=s.height;view.capturedAtUs=s.capturedAtUs;
        view.faceDetected=s.faceDetected;view.cropped=s.cropped;
        view.sourceWidth=s.sourceWidth;view.sourceHeight=s.sourceHeight;
        view.face=s.face;view.crop=s.crop;view.detectionScore=s.detectionScore;
        return true;
    }
    return false;
}
bool discardCameraImage(CameraImageHandle image) {
    using namespace camera_detail;
    if(!image)return true;
    ImageLock lock; if(!lock.locked)return false;
    for(auto &s:images)if(s.handle==image && s.state==ImageState::Ready){clearSlot(s);return true;}
    return false;
}
bool releaseCameraImage(CameraImageHandle image) {
    using namespace camera_detail;
    if(!image)return false;
    ImageLock lock; if(!lock.locked)return false;
    for(auto &s:images)if(s.handle==image && s.state==ImageState::Borrowed){clearSlot(s);return true;}
    return false;
}
const char *cameraErrorText(CameraError e) {
    switch(e) {
    case CameraError::FaceProcessingDisabled:return "Local face processing disabled";
    case CameraError::NoFace:return "No face found. Try again";
    case CameraError::MultipleFaces:return "Only one person in view";
    case CameraError::FaceTooSmall:return "Move closer and try again";
    case CameraError::FaceAtEdge:return "Center your whole face";
    case CameraError::FaceProcessingFailed:return "Face processing failed";
    case CameraError::Disabled:return "Camera hardware disabled";
    case CameraError::MissingPsram:return "Camera requires working PSRAM";
    case CameraError::InitFailed:return "Camera initialization failed";
    case CameraError::CaptureFailed:return "Camera capture failed";
    case CameraError::TimedOut:return "Camera capture timed out";
    case CameraError::InvalidJpeg:return "Camera returned an invalid image";
    case CameraError::StaleFrame:return "Camera timestamp invalid";
    case CameraError::NoMemory:return "Not enough image memory";
    case CameraError::ImagePoolBusy:return "Previous image still in use";
    default:return "Camera error";
    }
}
