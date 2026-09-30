#pragma once

// Temporary camera preview on a trusted LAN. The private URL uses unencrypted HTTP.
// Include only from camera_module.cpp; requests do not trigger new captures.
#include "HardwareConfig.h"
#include <esp_camera.h>
#include <stdint.h>
#ifndef KIOSK_CAMERA_PREVIEW
#define KIOSK_CAMERA_PREVIEW 1
#endif
#if KIOSK_CAMERA_PREVIEW && KIOSK_ENABLE_LOCK
#error "Camera preview is a bench diagnostic: keep KIOSK_ENABLE_LOCK=0 and unplug the 12V supply."
#endif

#if KIOSK_CAMERA_PREVIEW
#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

namespace camera_snapshot_preview {
constexpr uint16_t PORT = 8080;
constexpr uint32_t WINDOW_MS = 300000; // Five minutes from startup; new frames do not extend it.
constexpr uint32_t FRAME_TTL_MS = 120000; // Two minutes per snapshot.
constexpr size_t MAX_BYTES = 192U * 1024U;

struct Snapshot {
    uint8_t *data = nullptr;
    size_t length = 0;
    uint16_t width = 0, height = 0;
    uint32_t number = 0, generation = 0, storedMs = 0;
};
static Snapshot snapshot{}; // Protected by snapshotMutex.
static SemaphoreHandle_t snapshotMutex = nullptr;
static httpd_handle_t server = nullptr;
static bool attempted = false, retired = false;
static uint32_t openedMs = 0, nextNumber = 0;
static char pagePath[80] = {};
static char imagePath[96] = {};

inline void wipeFree(uint8_t *data, size_t length) {
    if (!data) return;
    volatile uint8_t *p = data;
    for (size_t i=0;i<length;++i) p[i]=0;
    heap_caps_free(data);
}
inline void clearLocked() {
    wipeFree(snapshot.data, snapshot.length);
    snapshot = Snapshot{};
}
inline bool windowOpen() { return millis() - openedMs < WINDOW_MS; }
inline bool freshLocked() {
    return snapshot.data && snapshot.length &&
           millis() - snapshot.storedMs < FRAME_TTL_MS && windowOpen();
}
inline void responseHeaders(httpd_req_t *req) {
    httpd_resp_set_hdr(req,"Cache-Control","no-store, private, max-age=0");
    httpd_resp_set_hdr(req,"Pragma","no-cache");
    httpd_resp_set_hdr(req,"Referrer-Policy","no-referrer");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(req,"Content-Security-Policy",
        "default-src 'none'; img-src 'self'; style-src 'unsafe-inline'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'");
}
inline esp_err_t textResponse(httpd_req_t *req,const char *status,const char *body) {
    responseHeaders(req);
    httpd_resp_set_status(req,status);
    httpd_resp_set_type(req,"text/plain; charset=utf-8");
    return httpd_resp_send(req,body,HTTPD_RESP_USE_STRLEN);
}
inline esp_err_t pageHandler(httpd_req_t *req) {
    if(!windowOpen())return textResponse(req,"410 Gone","Preview window expired. Restart the ESP32 for another temporary preview window.");
    if(!snapshotMutex || xSemaphoreTake(snapshotMutex,pdMS_TO_TICKS(20))!=pdTRUE)
        return textResponse(req,"503 Service Unavailable","Snapshot busy. Refresh this page.");
    const bool fresh=freshLocked();
    const uint32_t number=snapshot.number, generation=snapshot.generation;
    const uint32_t age=millis()-snapshot.storedMs;
    const unsigned width=snapshot.width,height=snapshot.height;
    const size_t bytes=snapshot.length;
    xSemaphoreGive(snapshotMutex);
    if(!fresh)return textResponse(req,"410 Gone","No current snapshot. Perform one FACE attempt, then refresh this page within two minutes.");
    char html[1900];
    const int n=snprintf(html,sizeof(html),
        "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 camera snapshot</title><style>body{font-family:system-ui;max-width:960px;margin:24px auto;padding:0 16px}"
        "img{max-width:100%%;height:auto;border:1px solid #888}code{overflow-wrap:anywhere}</style></head><body>"
        "<h1>Exact full camera frame</h1><p>This is a still image, not a live stream. It is shown with <b>no browser rotation or mirror correction</b>. "
        "Check whether your whole face is upright, clear and in frame.</p>"
        "<p>Snapshot %lu; operation generation %lu; %u x %u; %lu JPEG bytes; age when page loaded: %lu seconds.</p>"
        "<img src='%s?frame=%lu' alt='Snapshot expired or changed: refresh this page.'>"
        "<p><a href='%s?frame=%lu'>Open the JPEG alone</a></p>"
        "<p>After another FACE attempt, refresh this page. The snapshot is retained on the ESP32 for two minutes; "
        "the preview window lasts five minutes. Anyone holding this private link on your LAN can view it. "
        "HTTP is not encrypted. Do not share the link or use public Wi-Fi.</p>"
        "<p>This viewer does not enroll a person, change detector settings, or operate the lock. Close this tab after testing.</p></body></html>",
        static_cast<unsigned long>(number),static_cast<unsigned long>(generation),width,height,
        static_cast<unsigned long>(bytes),static_cast<unsigned long>(age/1000),imagePath,
        static_cast<unsigned long>(number),imagePath,static_cast<unsigned long>(number));
    if(n<0 || static_cast<size_t>(n)>=sizeof(html))return textResponse(req,"500 Internal Server Error","Cannot prepare preview page.");
    responseHeaders(req);
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    return httpd_resp_send(req,html,n);
}
inline bool requestedFrameNumber(httpd_req_t *req,uint32_t &number) {
    char query[64], value[16];
    if(httpd_req_get_url_query_str(req,query,sizeof(query))!=ESP_OK ||
       httpd_query_key_value(query,"frame",value,sizeof(value))!=ESP_OK || !value[0])return false;
    uint32_t parsed=0;
    for(size_t i=0;value[i];++i) {
        if(value[i]<'0'||value[i]>'9')return false;
        const uint32_t digit=static_cast<uint32_t>(value[i]-'0');
        if(parsed>(UINT32_MAX-digit)/10U)return false;
        parsed=parsed*10U+digit;
    }
    if(!parsed)return false;
    number=parsed;return true;
}
inline esp_err_t imageHandler(httpd_req_t *req) {
    if(!windowOpen())return textResponse(req,"410 Gone","Preview expired.");
    uint32_t wanted=0;
    if(!requestedFrameNumber(req,wanted))return textResponse(req,"400 Bad Request","Use the image link from the preview page.");
    if(!snapshotMutex || xSemaphoreTake(snapshotMutex,pdMS_TO_TICKS(20))!=pdTRUE)
        return textResponse(req,"503 Service Unavailable","Snapshot busy. Refresh the preview page.");
    if(!freshLocked() || wanted!=snapshot.number) {
        xSemaphoreGive(snapshotMutex);
        return textResponse(req,"410 Gone","Snapshot expired or changed. Refresh the preview page.");
    }
    // Copy for the HTTP task, then release the mutex before sending.
    const size_t bytes=snapshot.length;
    uint8_t *copy=static_cast<uint8_t*>(heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(copy)memcpy(copy,snapshot.data,bytes);
    xSemaphoreGive(snapshotMutex);
    if(!copy)return textResponse(req,"503 Service Unavailable","Not enough memory for preview; normal camera processing is unchanged.");
    responseHeaders(req);
    httpd_resp_set_type(req,"image/jpeg");
    const esp_err_t result=httpd_resp_send(req,reinterpret_cast<const char*>(copy),bytes);
    wipeFree(copy,bytes);
    return result;
}
inline bool ensureServer() {
    if(retired)return false;
    if(server)return windowOpen();
    if(attempted || WiFi.status()!=WL_CONNECTED)return false;
    attempted=true;
    snapshotMutex=xSemaphoreCreateMutex();
    if(!snapshotMutex){Serial.println("[CAMVIEW] Preview mutex allocation failed; detector continues without preview.");return false;}
    uint32_t randomWords[4];
    esp_fill_random(randomWords,sizeof(randomWords));
    snprintf(pagePath,sizeof(pagePath),"/camera-%08lx%08lx%08lx%08lx",
        static_cast<unsigned long>(randomWords[0]),static_cast<unsigned long>(randomWords[1]),
        static_cast<unsigned long>(randomWords[2]),static_cast<unsigned long>(randomWords[3]));
    snprintf(imagePath,sizeof(imagePath),"%s/image.jpg",pagePath);
    openedMs=millis();
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();
    config.server_port=PORT;
    config.ctrl_port=32770;
    config.task_priority=1;
    config.stack_size=6144;
    config.max_open_sockets=2;
    config.max_uri_handlers=2;
    config.lru_purge_enable=true;
    config.recv_wait_timeout=2;
    config.send_wait_timeout=2;
    esp_err_t err=httpd_start(&server,&config);
    if(err!=ESP_OK){server=nullptr;Serial.printf("[CAMVIEW] Preview start failed: 0x%x. Normal capture continues.\n",static_cast<unsigned>(err));return false;}
    httpd_uri_t page{};page.uri=pagePath;page.method=HTTP_GET;page.handler=pageHandler;
    httpd_uri_t image{};image.uri=imagePath;image.method=HTTP_GET;image.handler=imageHandler;
    if(httpd_register_uri_handler(server,&page)!=ESP_OK || httpd_register_uri_handler(server,&image)!=ESP_OK) {
        httpd_stop(server);server=nullptr;
        Serial.println("[CAMVIEW] Cannot register preview routes. Normal capture continues.");return false;
    }
    Serial.println("[CAMVIEW] Temporary PRIVATE-LAN snapshot viewer enabled. Link is private; HTTP is not encrypted.");
    return true;
}
inline void publish(const camera_fb_t &frame,uint32_t generation) {
    if(!frame.buf || frame.format!=PIXFORMAT_JPEG || frame.len<4 || frame.len>MAX_BYTES ||
       !ensureServer())return;
    if(xSemaphoreTake(snapshotMutex,0)!=pdTRUE){Serial.println("[CAMVIEW] Preview busy; skipping this copy only.");return;}
    clearLocked();
    uint8_t *copy=static_cast<uint8_t*>(heap_caps_malloc(frame.len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(copy) {
        memcpy(copy,frame.buf,frame.len);
        if(++nextNumber==0)++nextNumber;
        snapshot.data=copy;snapshot.length=frame.len;snapshot.width=frame.width;snapshot.height=frame.height;
        snapshot.number=nextNumber;snapshot.generation=generation;snapshot.storedMs=millis();
    }
    const uint32_t number=snapshot.number;
    xSemaphoreGive(snapshotMutex);
    if(!copy){Serial.println("[CAMVIEW] No memory for preview copy; detector continues.");return;}
    Serial.printf("[CAMVIEW] Snapshot %lu, generation %lu. Open on a device on the SAME private Wi-Fi:\n",
        static_cast<unsigned long>(number),static_cast<unsigned long>(generation));
    Serial.printf("[CAMVIEW] http://%s:%u%s\n",WiFi.localIP().toString().c_str(),static_cast<unsigned>(PORT),pagePath);
}
// Preview expiry never extends an authentication session.
inline void service(bool cameraBusy) {
    if(!server)return;
    if(snapshotMutex && xSemaphoreTake(snapshotMutex,0)==pdTRUE) {
        if(snapshot.data && !freshLocked())clearLocked();
        xSemaphoreGive(snapshotMutex);
    }
    // Handlers reject expired links even if capture delays server shutdown.
    if(!windowOpen() && !cameraBusy) {
        httpd_stop(server);server=nullptr;retired=true;
        Serial.println("[CAMVIEW] Five-minute preview window closed. Restart only if another diagnostic is needed.");
    }
}
}
#else
namespace camera_snapshot_preview {
inline void service(bool) {}
inline void publish(const camera_fb_t &,uint32_t) {}
}
#endif
