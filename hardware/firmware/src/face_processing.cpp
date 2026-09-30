#include "face_processing.h"
#include "HardwareConfig.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <img_converters.h>
#include <esp_arduino_version.h>
#include <math.h>
#include <string.h>
#include <new>

// Temporary metadata-only diagnostics. No image bytes, IDs, PINs or session tokens.
// This patch does NOT change inference thresholds, orientation or authorization.
#ifndef KIOSK_CAMERA_DIAGNOSTICS
#define KIOSK_CAMERA_DIAGNOSTICS 1
#endif
#include <stdio.h>
#include <stdarg.h>


// This adapter deliberately targets the bundled legacy ESP-DL in Arduino 2.0.17.
// Do not silently substitute Arduino 3.x / current ESP-WHO (different APIs/models).
#if ESP_ARDUINO_VERSION_MAJOR != 2 || ESP_ARDUINO_VERSION_MINOR != 0 || ESP_ARDUINO_VERSION_PATCH != 17
#error "Face processing targets Arduino-ESP32 2.0.17. Use platform espressif32@6.12.0; see README."
#endif
#if !CONFIG_IDF_TARGET_ESP32S3
#error "This face adapter is for ESP32-S3."
#endif
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"

namespace {
void faceDiagnostic(const char *format, ...) {
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
void wipeFree(uint8_t *p, size_t n) {
    if (!p) return;
    volatile uint8_t *v=p; for(size_t i=0;i<n;++i)v[i]=0;
    heap_caps_free(p);
}
struct Buffer {
    uint8_t *data=nullptr; size_t bytes=0;
    bool allocate(size_t n) {
        data=static_cast<uint8_t*>(heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(!data)return false;
        bytes=n; memset(data,0,n); return true;
    }
    ~Buffer(){wipeFree(data,bytes);}
    Buffer()=default; Buffer(const Buffer&)=delete; Buffer&operator=(const Buffer&)=delete;
};
bool stopped(FaceStopRequested stop,void *context){return stop && stop(context);}

struct JpegOutput {
    uint8_t *data; size_t capacity; size_t length; bool failed;
};
size_t jpegChunk(void *arg,size_t index,const void *data,size_t length) {
    auto &out=*static_cast<JpegOutput*>(arg);
    if(out.failed || !data || index!=out.length || index>out.capacity ||
       length>out.capacity-index){out.failed=true;return 0;}
    memcpy(out.data+index,data,length);out.length+=length;return length;
}
// Follows Espressif's 2.0.17 CameraWebServer pipeline: fmt2rgb888 returns its
// BGR888 image buffer, which that example passes straight to uint8_t inference
// and PIXFORMAT_RGB888 encoding. Do not swap channels in only one of these stages.
void downsample2(const uint8_t *source,uint8_t *dest,unsigned w,unsigned h) {
    for(unsigned y=0;y<h/2;++y)for(unsigned x=0;x<w/2;++x)for(unsigned c=0;c<3;++c){
        const size_t p=(static_cast<size_t>(2*y)*w+2*x)*3+c;
        dest[(static_cast<size_t>(y)*(w/2)+x)*3+c]=static_cast<uint8_t>(
            (static_cast<unsigned>(source[p])+source[p+3]+source[p+w*3]+source[p+w*3+3]+2)/4);
    }
}
} // namespace

bool jpegDimensionsMatch(const uint8_t *p,size_t n,uint16_t width,uint16_t height) {
    if(!p || n<4 || p[0]!=0xFF || p[1]!=0xD8 || p[n-2]!=0xFF || p[n-1]!=0xD9)return false;
    bool found=false;size_t pos=2;
    while(pos<n){
        if(p[pos++]!=0xFF)return false;
        while(pos<n && p[pos]==0xFF)++pos;
        if(pos>=n)return false;
        const uint8_t marker=p[pos++];
        if(marker==0xD9)return false; // No scan data.
        if(marker==0xDA)return found; // SOS: dimensions checked before decoder writes.
        if(marker==0x00 || marker==0xD8 || (marker>=0xD0 && marker<=0xD7))return false;
        if(marker==0x01)continue;
        if(pos+2>n)return false;
        const size_t length=(static_cast<size_t>(p[pos])<<8)|p[pos+1];
        if(length<2 || length>n-pos)return false;
        const bool isSOF=marker>=0xC0 && marker<=0xCF && marker!=0xC4 && marker!=0xC8 && marker!=0xCC;
        if(isSOF){
            // OV2640 baseline JPEG only; reject progressive/12-bit/mismatched headers.
            if(marker!=0xC0 || found || length<17 || p[pos+2]!=8 || p[pos+7]!=3)return false;
            const uint16_t h=(static_cast<uint16_t>(p[pos+3])<<8)|p[pos+4];
            const uint16_t w=(static_cast<uint16_t>(p[pos+5])<<8)|p[pos+6];
            if(w!=width || h!=height)return false;
            found=true;
        }
        pos+=length;
    }
    return false;
}
FaceProcessingError faceRectangles(int l,int t,int r,int b,uint16_t dw,uint16_t dh,
                                  uint16_t sw,uint16_t sh,FaceRectangle &face,FaceRectangle &crop){
    face=FaceRectangle{};crop=FaceRectangle{};
    if(!dw||!dh||!sw||!sh||l>r||t>b)return FaceProcessingError::InvalidDetection;
    if(l<0||t<0||r>=dw||b>=dh)return FaceProcessingError::FaceAtEdge;
    const unsigned x=static_cast<unsigned>(l)*sw/dw;
    const unsigned y=static_cast<unsigned>(t)*sh/dh;
    const unsigned endX=(static_cast<unsigned>(r+1)*sw+dw-1)/dw;
    const unsigned endY=(static_cast<unsigned>(b+1)*sh+dh-1)/dh;
    const unsigned w=endX-x,h=endY-y;
    if(w<HardwareConfig::FACE_MIN_SIDE_PX || h<HardwareConfig::FACE_MIN_SIDE_PX)
        return FaceProcessingError::FaceTooSmall;
    face.x=x;face.y=y;face.width=w;face.height=h;
    const unsigned mx=(w*HardwareConfig::FACE_CROP_MARGIN_PERCENT+99)/100;
    const unsigned my=(h*HardwareConfig::FACE_CROP_MARGIN_PERCENT+99)/100;
    crop.x=x>mx?x-mx:0;crop.y=y>my?y-my:0;
    const unsigned ex=endX+mx<sw?endX+mx:sw,ey=endY+my<sh?endY+my:sh;
    crop.width=ex-crop.x;crop.height=ey-crop.y;
    return FaceProcessingError::None;
}
FaceProcessingError detectAndCropFace(const uint8_t *jpeg,size_t length,uint16_t w,uint16_t h,
                                     ProcessedFaceImage &out,FaceStopRequested stop,void *context){
    // Caller must release a previous output, rather than leaking it by overwrite.
    if(out.data)return FaceProcessingError::InvalidJpeg;
    out=ProcessedFaceImage{};
    const uint32_t processingStarted=millis();
    faceDiagnostic("[FACEDBG] start t=%lu ms; input=%ux%u JPEG=%lu bytes",
        static_cast<unsigned long>(processingStarted), static_cast<unsigned>(w),
        static_cast<unsigned>(h), static_cast<unsigned long>(length));
    if(!HardwareConfig::FACE_PROCESSING_ENABLED)return FaceProcessingError::Disabled;
    if(stopped(stop,context))return FaceProcessingError::Cancelled;
    if(w!=640||h!=480||length>HardwareConfig::CAMERA_MAX_JPEG_BYTES||
       !jpegDimensionsMatch(jpeg,length,w,h))return FaceProcessingError::InvalidJpeg;
    if(!psramFound())return FaceProcessingError::NoMemory;
    Buffer full,small;
    if(!full.allocate(static_cast<size_t>(w)*h*3)||!small.allocate(static_cast<size_t>(w/2)*(h/2)*3))
        return FaceProcessingError::NoMemory;
    const uint32_t decodeStarted=millis();
    if(!fmt2rgb888(jpeg,length,PIXFORMAT_JPEG,full.data)) {
        faceDiagnostic("[FACEDBG] JPEG decoding FAILED");
        return FaceProcessingError::DecodeFailed;
    }
    faceDiagnostic("[FACEDBG] JPEG decoded in %lu ms",
        static_cast<unsigned long>(millis()-decodeStarted));
    if(stopped(stop,context))return FaceProcessingError::Cancelled;
    downsample2(full.data,small.data,w,h);
#if KIOSK_CAMERA_DIAGNOSTICS
    // Sparse raw-channel statistics only: not a quality score or a face check.
    unsigned minimum=255, maximum=0;
    uint32_t channelSum[3]={0,0,0};
    size_t samples=0;
    const size_t pixelCount=static_cast<size_t>(w/2)*(h/2);
    for(size_t p=0;p<pixelCount;p+=16) {
        for(unsigned c=0;c<3;++c) {
            const unsigned value=small.data[p*3+c];
            if(value<minimum)minimum=value;
            if(value>maximum)maximum=value;
            channelSum[c]+=value;
        }
        ++samples;
    }
    faceDiagnostic("[FACEDBG] detector image=%ux%u; sampled byte min=%u max=%u; channel means=%lu,%lu,%lu (0..255)",
        static_cast<unsigned>(w/2),static_cast<unsigned>(h/2),minimum,maximum,
        static_cast<unsigned long>(channelSum[0]/samples),
        static_cast<unsigned long>(channelSum[1]/samples),
        static_cast<unsigned long>(channelSum[2]/samples));
#endif

    // Construct on the camera worker's first inference, never at global startup.
    // Internal model allocations belong to ESP-DL, not the JPEG lease pool.
    static HumanFaceDetectMSR01 stage1(0.1F,0.5F,10,0.2F);
    static HumanFaceDetectMNP01 stage2(HardwareConfig::FACE_SCORE_THRESHOLD,0.3F,5);
    const uint32_t firstStageStarted=millis();
    auto &candidates=stage1.infer(small.data,{h/2,w/2,3});
    faceDiagnostic("[FACEDBG] stage1 candidates=%lu; stage1 elapsed=%lu ms",
        static_cast<unsigned long>(candidates.size()),
        static_cast<unsigned long>(millis()-firstStageStarted));
    if(stopped(stop,context))return FaceProcessingError::Cancelled;
    const uint32_t secondStageStarted=millis();
    auto &results=stage2.infer(small.data,{h/2,w/2,3},candidates);
    faceDiagnostic("[FACEDBG] stage2 faces=%lu; stage2 elapsed=%lu ms; processing elapsed=%lu ms",
        static_cast<unsigned long>(results.size()),
        static_cast<unsigned long>(millis()-secondStageStarted),
        static_cast<unsigned long>(millis()-processingStarted));
    if(stopped(stop,context))return FaceProcessingError::Cancelled;
    if(results.empty()) {
        faceDiagnostic("[FACEDBG] LOCAL_NO_FACE: no accepted local detection; no face crop produced");
        return FaceProcessingError::NoFace;
    }
    if(results.size()!=1)return FaceProcessingError::MultipleFaces; // No arbitrary largest-face selection.
    const auto &result=results.front();
    if(result.box.size()!=4 || !isfinite(result.score)||result.score<HardwareConfig::FACE_SCORE_THRESHOLD)
        return FaceProcessingError::InvalidDetection;
    FaceRectangle face{},crop{};
    const auto error=faceRectangles(result.box[0],result.box[1],result.box[2],result.box[3],
                                    w/2,h/2,w,h,face,crop);
    if(error!=FaceProcessingError::None)return error;
    Buffer pixels,encoded;
    if(!pixels.allocate(static_cast<size_t>(crop.width)*crop.height*3)||
       !encoded.allocate(HardwareConfig::CAMERA_MAX_JPEG_BYTES))return FaceProcessingError::NoMemory;
    for(unsigned y=0;y<crop.height;++y){
        memcpy(pixels.data+static_cast<size_t>(y)*crop.width*3,
               full.data+(static_cast<size_t>(crop.y+y)*w+crop.x)*3,static_cast<size_t>(crop.width)*3);
        if((y%16)==0&&stopped(stop,context))return FaceProcessingError::Cancelled;
    }
    JpegOutput encoder{encoded.data,encoded.bytes,0,false};
    const bool ok=fmt2jpg_cb(pixels.data,pixels.bytes,crop.width,crop.height,PIXFORMAT_RGB888,
                             HardwareConfig::FACE_JPEG_QUALITY,jpegChunk,&encoder);
    if(stopped(stop,context))return FaceProcessingError::Cancelled;
    if(!ok||encoder.failed||encoder.length<4||
       !jpegDimensionsMatch(encoded.data,encoder.length,crop.width,crop.height))
        return FaceProcessingError::EncodeFailed;
    out.data=encoded.data;out.length=encoder.length;out.allocatedBytes=encoded.bytes;
    encoded.data=nullptr;encoded.bytes=0; // Transfer ownership only on complete success.
    out.width=crop.width;out.height=crop.height;out.sourceWidth=w;out.sourceHeight=h;
    out.face=face;out.crop=crop;out.detectionScore=result.score;
    out.faceDetected=true;out.cropped=true;
    faceDiagnostic("[FACEDBG] face crop ready %ux%u, JPEG=%lu bytes; total processing=%lu ms",
        static_cast<unsigned>(out.width),static_cast<unsigned>(out.height),
        static_cast<unsigned long>(out.length),
        static_cast<unsigned long>(millis()-processingStarted));
    return FaceProcessingError::None;
}
void freeProcessedFaceImage(ProcessedFaceImage &image){wipeFree(image.data,image.allocatedBytes);image=ProcessedFaceImage{};}
const char *faceProcessingErrorText(FaceProcessingError e){
    switch(e){
    case FaceProcessingError::Disabled:return "Local face processing disabled";
    case FaceProcessingError::NoFace:return "No face found. Try again";
    case FaceProcessingError::MultipleFaces:return "Only one person in view";
    case FaceProcessingError::FaceTooSmall:return "Move closer and try again";
    case FaceProcessingError::FaceAtEdge:return "Center your whole face";
    case FaceProcessingError::NoMemory:return "Face processing needs PSRAM";
    case FaceProcessingError::DecodeFailed:return "Cannot decode camera image";
    case FaceProcessingError::EncodeFailed:return "Cannot encode face crop";
    case FaceProcessingError::Cancelled:return "Face capture cancelled";
    default:return "Invalid face image";
    }
}
