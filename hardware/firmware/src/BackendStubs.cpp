// HTTPS client; requests run on the backend worker.
#include "BackendInterface.h"
#include "BackendJson.h"
#include "NetworkConfig.h"
#include "BackendPolicy.h"
#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_log.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/version.h>
#include <sdkconfig.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Avoid the conflicting Arduino header; use the SDK certificate-bundle API.
extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

#if __has_include("secrets.h")
#include "secrets.h"
#else
namespace KioskSecrets {
constexpr const char *WIFI_SSID="";
constexpr const char *WIFI_PASSWORD="";
constexpr const char *DEVICE_API_KEY="";
}
#endif
#if !defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) || !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "This client requires the ESP32 certificate bundle; keep espressif32@6.12.0. Do not disable TLS verification."
#endif
#if MBEDTLS_VERSION_MAJOR != 2
#error "The v8 TLS adapter targets Arduino-ESP32 2.0.17/mbedTLS 2.x. Review it before changing the SDK."
#endif

namespace network_detail {
using namespace NetworkConfig;
using backend_json::Operation;
bool wifiStarted=false, clockStarted=false;
uint32_t lastWifiAttempt=0;
int lastNetworkMessage=-1;

void message(int id, const char *text) {
    if (id==lastNetworkMessage) return;
    lastNetworkMessage=id; Serial.println(text);
}
void wipe(void *p,size_t n) { volatile unsigned char *q=static_cast<volatile unsigned char*>(p); while(n--)*q++=0; }
bool localConfigValid() {
    const size_t ssid=strlen(KioskSecrets::WIFI_SSID), key=strlen(KioskSecrets::DEVICE_API_KEY);
    if (!ssid || ssid>32 || strlen(KioskSecrets::WIFI_PASSWORD)>64 || !key || key>512) return false;
    for(const char *s=KioskSecrets::DEVICE_API_KEY;*s;++s)
        if(static_cast<unsigned char>(*s)<=32 || static_cast<unsigned char>(*s)>126 || *s=='<' || *s=='>')return false;
    return true;
}
bool saneClock() { const time_t now=time(nullptr); return now>=1735689600LL && now<4102444800LL; }
void service() {
    if (!KIOSK_ENABLE_NETWORK) { message(0,"[NET] Disabled by KIOSK_ENABLE_NETWORK."); return; }
    if (!localConfigValid()) { message(1,"[NET] Fill local src/secrets.h; no requests will be sent."); return; }
    if (!wifiStarted) {
        // HTTP debug logs may expose credentials.
        esp_log_level_set("HTTP_CLIENT",ESP_LOG_NONE);
        WiFi.persistent(false); WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
        WiFi.begin(KioskSecrets::WIFI_SSID,KioskSecrets::WIFI_PASSWORD);
        wifiStarted=true; lastWifiAttempt=millis();
    }
    if (WiFi.status()!=WL_CONNECTED) {
        message(2,"[NET] Connecting to Wi-Fi...");
        if (millis()-lastWifiAttempt>=WIFI_RETRY_MS) {
            lastWifiAttempt=millis(); WiFi.reconnect();
        }
        return;
    }
    if (!clockStarted) { configTime(0,0,NTP_PRIMARY,NTP_SECONDARY); clockStarted=true; }
    if (!saneClock()) { message(3,"[NET] Wi-Fi connected; synchronizing clock..."); return; }
    message(4,"[NET] Wi-Fi and clock ready. Waiting for ID/PIN or sensor input.");
}
void fail(uint32_t id,const char *reason) {
    BackendResponse r{};r.requestId=id;copyBackendText(r.failureReason,reason);
    if (!handleBackendResponse(r)) Serial.println("[NET] Response queue full; controller will time out.");
}

// Add UTC validity checks to the SDK bundle verifier.
struct ChainVerifier {
    int (*callback)(void *,mbedtls_x509_crt *,int,uint32_t *)=nullptr;
    void *context=nullptr;
} chainVerifier;
int compareTime(const mbedtls_x509_time &a,const mbedtls_x509_time &b) {
    const int x[]={a.year,a.mon,a.day,a.hour,a.min,a.sec};
    const int y[]={b.year,b.mon,b.day,b.hour,b.min,b.sec};
    for(unsigned i=0;i<6;++i)if(x[i]!=y[i])return x[i]<y[i]?-1:1;
    return 0;
}
int verifyDates(void *p,mbedtls_x509_crt *crt,int depth,uint32_t *flags) {
    ChainVerifier *chain=static_cast<ChainVerifier*>(p);
    if (!chain || !chain->callback || !crt || !flags)return -1;
    const int result=chain->callback(chain->context,crt,depth,flags);
    struct tm utc{};time_t epoch=time(nullptr);
    if (!saneClock() || !gmtime_r(&epoch,&utc)) { *flags|=MBEDTLS_X509_BADCERT_OTHER; return result; }
    const mbedtls_x509_time now{utc.tm_year+1900,utc.tm_mon+1,utc.tm_mday,utc.tm_hour,utc.tm_min,utc.tm_sec};
    if(compareTime(now,crt->valid_from)<0)*flags|=MBEDTLS_X509_BADCERT_FUTURE;
    if(compareTime(now,crt->valid_to)>0)*flags|=MBEDTLS_X509_BADCERT_EXPIRED;
    return result;
}
esp_err_t attachVerifiedBundle(void *p) {
    esp_err_t result = ::esp_crt_bundle_attach(p);
    if(result!=ESP_OK)return result;
    auto *conf=static_cast<mbedtls_ssl_config*>(p);
    if(!conf || !conf->f_vrfy || conf->f_vrfy==verifyDates)return ESP_FAIL;
    chainVerifier.callback=conf->f_vrfy;chainVerifier.context=conf->p_vrfy;
    mbedtls_ssl_conf_verify(conf,verifyDates,&chainVerifier);
    return ESP_OK;
}

struct Transfer {
    uint32_t requestId=0, startedMs=0;
    char contentType[96]={};
    bool badHeaders=false;
};
bool live(const Transfer &t) {
    return millis()-t.startedMs<REQUEST_BUDGET_MS && isBackendRequestCurrent(t.requestId);
}
esp_err_t onHttpEvent(esp_http_client_event_t *event) {
    auto *t=static_cast<Transfer*>(event->user_data);
    if(!t)return ESP_FAIL;
    if(event->event_id==HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
        if(strcasecmp(event->header_key,"Content-Type")==0) {
            if(strlen(event->header_value)>=sizeof(t->contentType))t->badHeaders=true;
            else if(t->contentType[0] && strcasecmp(t->contentType,event->header_value)!=0)t->badHeaders=true;
            else strcpy(t->contentType,event->header_value);
        }
        if(strcasecmp(event->header_key,"Content-Encoding")==0 &&
           strcasecmp(event->header_value,"identity")!=0)t->badHeaders=true;
    }
    return ESP_OK;
}
struct Client {
    esp_http_client_handle_t handle=nullptr;
    ~Client(){if(handle)esp_http_client_cleanup(handle);}
    Client(const Client&)=delete;Client&operator=(const Client&)=delete;
    Client()=default;
};
struct ReplyBuffer {
    char *data=static_cast<char*>(calloc(MAX_RESPONSE_BYTES+2,1));
    ~ReplyBuffer(){if(data){wipe(data,MAX_RESPONSE_BYTES+2);free(data);}}
};
struct JsonBody {
    cJSON *object=cJSON_CreateObject();
    char text[MAX_REQUEST_JSON_BYTES]={};
    ~JsonBody(){wipe(text,sizeof(text));if(object){
        cJSON *pin=cJSON_GetObjectItemCaseSensitive(object,"pin");
        if(cJSON_IsString(pin)&&pin->valuestring)wipe(pin->valuestring,strlen(pin->valuestring));
        cJSON_Delete(object);
    }}
    bool add(const char *key,const char *value){return object && cJSON_AddStringToObject(object,key,value)!=nullptr;}
    bool addSlot(const char *key,int value,bool nullIfNegative){
        return object && (value<0&&nullIfNegative ? cJSON_AddNullToObject(object,key)!=nullptr
                                                : cJSON_AddNumberToObject(object,key,value)!=nullptr);
    }
    bool encode(){return object && cJSON_PrintPreallocated(object,text,sizeof(text),0);}
};
struct Payload {
    const char *prefix=nullptr;
    const uint8_t *image=nullptr;
    size_t imageLength=0;
    const char *suffix=nullptr;
    const char *contentType="application/json";
};
bool writeAll(Client &client,Transfer &t,const uint8_t *data,size_t length) {
    size_t sent=0;
    while(sent<length) {
        if(!live(t))return false;
        const int count=static_cast<int>(std::min(length-sent,static_cast<size_t>(2048)));
        const int n=esp_http_client_write(client.handle,reinterpret_cast<const char*>(data+sent),count);
        if(n<=0 || n>count)return false;
        sent+=static_cast<size_t>(n);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}
const char *httpError(int status) {
    if(status==401 || status==403)return "Device key rejected";
    if(status==404)return "API endpoint not found";
    if(status==413)return "Image too large for server";
    if(status==422)return "API fields rejected (422)";
    if(status==429)return "Too many requests; wait";
    if(status>=500)return "Server error; result may be unknown";
    if(status>=300 && status<400)return "API redirected; request stopped";
    return "Invalid server response";
}
void perform(Operation op,const char *path,const BackendRequestContext &ctx,
             const char *wireId,const Payload &payload) {
    service();
    if(!KIOSK_ENABLE_NETWORK){fail(ctx.requestId,"Network disabled");return;}
    if(!localConfigValid()){fail(ctx.requestId,"Configure Wi-Fi and device key");return;}
    if(WiFi.status()!=WL_CONNECTED){fail(ctx.requestId,"Wi-Fi not connected; wait");return;}
    if(!saneClock()){fail(ctx.requestId,"Waiting for clock sync");return;}
    if(!isBackendRequestCurrent(ctx.requestId))return;
    const size_t prefixSize=payload.prefix?strlen(payload.prefix):0;
    const size_t suffixSize=payload.suffix?strlen(payload.suffix):0;
    if(payload.imageLength>MAX_JPEG_BYTES || prefixSize>2048 || suffixSize>256 ||
       (payload.imageLength && !payload.image)){fail(ctx.requestId,"Request payload invalid");return;}
    const size_t total=prefixSize+payload.imageLength+suffixSize;
    if(!total){fail(ctx.requestId,"Request body missing");return;}
    char url[256];int urlSize=snprintf(url,sizeof(url),"%s%s",BASE_URL,path);
    if(urlSize<0 || static_cast<size_t>(urlSize)>=sizeof(url)){fail(ctx.requestId,"API URL invalid");return;}
    Transfer transfer{};transfer.requestId=ctx.requestId;transfer.startedMs=millis();
    esp_http_client_config_t cfg{};
    cfg.url=url;cfg.method=HTTP_METHOD_POST;cfg.timeout_ms=SOCKET_TIMEOUT_MS;
    cfg.transport_type=HTTP_TRANSPORT_OVER_SSL;
    cfg.crt_bundle_attach=attachVerifiedBundle;cfg.skip_cert_common_name_check=false;
    cfg.disable_auto_redirect=true;cfg.max_authorization_retries=-1;
    cfg.event_handler=onHttpEvent;cfg.user_data=&transfer;
    cfg.buffer_size=1024;cfg.buffer_size_tx=1024;
    cfg.user_agent="Freenove-Kiosk/8";
    Client client;client.handle=esp_http_client_init(&cfg);
    if(!client.handle){fail(ctx.requestId,"HTTPS allocation failed");return;}
    if(esp_http_client_set_header(client.handle,"X-Device-Key",KioskSecrets::DEVICE_API_KEY)!=ESP_OK ||
       esp_http_client_set_header(client.handle,"Content-Type",payload.contentType)!=ESP_OK ||
       esp_http_client_set_header(client.handle,"Accept","application/json")!=ESP_OK ||
       esp_http_client_set_header(client.handle,"Accept-Encoding","identity")!=ESP_OK ||
       esp_http_client_set_header(client.handle,"Connection","close")!=ESP_OK) {
        fail(ctx.requestId,"HTTPS header setup failed");return;
    }
    ReplyBuffer reply;
    if(!reply.data){fail(ctx.requestId,"Response allocation failed");return;}
    char log[160];snprintf(log,sizeof(log),"[NET] Sending %s %s",wireId,path);Serial.println(log);
    if(!live(transfer))return;
    esp_err_t opened=esp_http_client_open(client.handle,static_cast<int>(total));
    // A timeout may follow a committed request, so do not retry automatically.
    if(opened!=ESP_OK){fail(ctx.requestId,"HTTPS connection failed; check network/TLS");return;}
    if(!writeAll(client,transfer,reinterpret_cast<const uint8_t*>(payload.prefix),prefixSize) ||
       !writeAll(client,transfer,payload.image,payload.imageLength) ||
       !writeAll(client,transfer,reinterpret_cast<const uint8_t*>(payload.suffix),suffixSize)) {
        fail(ctx.requestId,"Upload interrupted; result may be unknown");return;
    }
    if(!live(transfer))return;
    const auto headerLength=esp_http_client_fetch_headers(client.handle);
    const int status=esp_http_client_get_status_code(client.handle);
    snprintf(log,sizeof(log),"[NET] %s HTTP %d",wireId,status);Serial.println(log);
    if(headerLength<0){fail(ctx.requestId,"Response missing; result may be unknown");return;}
    if(status>=300 && status<400){fail(ctx.requestId,httpError(status));return;}
    if(status==401 || status==403){fail(ctx.requestId,httpError(status));return;}
    if(transfer.badHeaders || strncasecmp(transfer.contentType,"application/json",16)!=0 ||
       (transfer.contentType[16] && transfer.contentType[16]!=';' && transfer.contentType[16]!=' ')) {
        fail(ctx.requestId,httpError(status));return;
    }
    const auto declared=esp_http_client_get_content_length(client.handle);
    if(declared>static_cast<int64_t>(MAX_RESPONSE_BYTES)){fail(ctx.requestId,"Server response too large");return;}
    size_t used=0;
    while(true) {
        if(!live(transfer))return;
        // Read bytes cached while parsing the response headers.
        const int room=static_cast<int>(std::min(MAX_RESPONSE_BYTES+1-used,static_cast<size_t>(1024)));
        int n=esp_http_client_read(client.handle,reply.data+used,room);
        if(n<0 || n>room){fail(ctx.requestId,"Response interrupted; result unknown");return;}
        used+=static_cast<size_t>(n);
        if(used>MAX_RESPONSE_BYTES){fail(ctx.requestId,"Server response too large");return;}
        if(n==0 && esp_http_client_is_complete_data_received(client.handle))break;
        if(n==0)vTaskDelay(pdMS_TO_TICKS(10));
    }
    reply.data[used]=0;
    if(!live(transfer))return;
    BackendResponse response{};
    if(!backend_json::decode(reply.data,used,op,ctx,wireId,status,response)) {
        fail(ctx.requestId,(status<200 || status>=300)?httpError(status):"Invalid or conflicting API response");return;
    }
    snprintf(log,sizeof(log),"[NET] Result: %s; flow=%s; next=%s",
             response.success?"success":"rejected",response.flow,response.nextStep);
    Serial.println(log);
    if(!handleBackendResponse(response))Serial.println("[NET] Response queue full; controller will time out.");
}
bool context(uint32_t id,BackendRequestContext &ctx,char (&wire)[48]) {
    if(!getBackendRequestContext(id,ctx) || !isBackendRequestCurrent(id))return false;
    snprintf(wire,sizeof(wire),"req-%08lx%08lx-%lu",
        static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(id));
    return true;
}
void jsonRequest(Operation op,const char *path,uint32_t id,const char *session,
                 const char *text,const char *pin,int slot=0) {
    BackendRequestContext ctx{};char wire[48];if(!context(id,ctx,wire))return;
    JsonBody body;bool ok=body.add("request_id",wire);
    switch(op) {
        case Operation::IdPin:
            ok=ok&&body.add("employee_id",text)&&body.add("pin",pin);break;
        case Operation::RfidAuth: ok=ok&&body.add("rfid_uid",text);break;
        case Operation::FingerprintAuth:
            ok=ok&&session&&strcmp(session,ctx.authSessionId)==0&&BackendPolicy::validFingerprintMatch(slot)&&
                body.add("session_id",session)&&body.addSlot("matched_template_slot",slot,true);break;
        case Operation::EnrollRfid:
            ok=ok&&session&&strcmp(session,ctx.enrollmentSessionId)==0&&
                body.add("enrollment_session_id",session)&&body.add("rfid_uid",text);break;
        case Operation::EnrollFingerprint:
            ok=ok&&session&&strcmp(session,ctx.enrollmentSessionId)==0&&BackendPolicy::validFingerprintSlot(slot)&&
                body.add("enrollment_session_id",session)&&body.addSlot("template_slot",slot,false);break;
        case Operation::ConfirmOldDeleted:
            ok=ok&&session&&strcmp(session,ctx.enrollmentSessionId)==0&&body.add("enrollment_session_id",session);break;
        default:ok=false;break;
    }
    if(!ok || !body.encode()){fail(id,"Cannot encode request");return;}
    Payload payload{};payload.prefix=body.text;
    perform(op,path,ctx,wire,payload);
}
void faceRequest(Operation op,const char *path,const char *session,uint32_t id) {
    BackendRequestContext ctx{};char wire[48];if(!context(id,ctx,wire))return;
    CameraImageView image{};
    const bool auth=op==Operation::FaceAuth;
    const char *expected=auth?ctx.authSessionId:ctx.enrollmentSessionId;
    if(!session || strcmp(session,expected)!=0 || !backend_json::cleanText(session) ||
       !getBackendFaceImage(id,image) || !image.data || image.length<4 ||
       image.length>MAX_JPEG_BYTES || !image.faceDetected || !image.cropped ||
       image.data[0]!=0xFF || image.data[1]!=0xD8) {
        fail(id,"Face image or session invalid");return;
    }
    char boundary[72];snprintf(boundary,sizeof(boundary),"kiosk-%s",wire);
    char prefix[1024],suffix[96],contentType[128];
    int n=snprintf(prefix,sizeof(prefix),
        "--%s\r\nContent-Disposition: form-data; name=\"request_id\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"face.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        boundary,wire,boundary,auth?"session_id":"enrollment_session_id",session,boundary);
    if(n<0 || static_cast<size_t>(n)>=sizeof(prefix)){fail(id,"Multipart header too long");return;}
    snprintf(suffix,sizeof(suffix),"\r\n--%s--\r\n",boundary);
    snprintf(contentType,sizeof(contentType),"multipart/form-data; boundary=%s",boundary);
    Payload payload{};payload.prefix=prefix;payload.image=image.data;payload.imageLength=image.length;
    payload.suffix=suffix;payload.contentType=contentType;
    perform(op,path,ctx,wire,payload); // The caller releases the JPEG after this synchronous request.
    wipe(prefix,sizeof(prefix));
}
}

void serviceBackendNetwork(){network_detail::service();}
void sendIdPinToBackend(String id,String pin,uint32_t requestId){
    network_detail::jsonRequest(backend_json::Operation::IdPin,"/kiosk/id-pin",requestId,nullptr,id.c_str(),pin.c_str());
}
void sendRfidAuthToBackend(String uid,uint32_t id){
    network_detail::jsonRequest(backend_json::Operation::RfidAuth,"/auth/rfid",id,nullptr,uid.c_str(),nullptr);
}
void sendFaceAuthToBackend(String session,uint32_t id){
    network_detail::faceRequest(backend_json::Operation::FaceAuth,"/auth/face",session.c_str(),id);
}
void sendFingerprintAuthToBackend(String session,int slot,uint32_t id){
    network_detail::jsonRequest(backend_json::Operation::FingerprintAuth,"/auth/fingerprint",id,session.c_str(),nullptr,nullptr,slot);
}
void sendEnrollmentRfid(String session,String uid,uint32_t id){
    network_detail::jsonRequest(backend_json::Operation::EnrollRfid,"/enroll/rfid",id,session.c_str(),uid.c_str(),nullptr);
}
void sendEnrollmentFace(String session,uint32_t id){
    network_detail::faceRequest(backend_json::Operation::EnrollFace,"/enroll/face",session.c_str(),id);
}
void sendEnrollmentFingerprint(String session,int slot,uint32_t id){
    network_detail::jsonRequest(backend_json::Operation::EnrollFingerprint,"/enroll/fingerprint",id,session.c_str(),nullptr,nullptr,slot);
}
void confirmOldFingerprintsDeleted(String session,uint32_t id){
    network_detail::jsonRequest(backend_json::Operation::ConfirmOldDeleted,"/enroll/fingerprint/confirm-old-deleted",id,session.c_str(),nullptr,nullptr);
}
