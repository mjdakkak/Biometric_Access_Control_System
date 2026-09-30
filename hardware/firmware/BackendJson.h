#pragma once
// Validate server JSON before passing it to the controller.
#include "BackendInterface.h"
#include "NetworkConfig.h"
#include "BackendPolicy.h"
#include <cJSON.h>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace backend_json {
enum class Operation { IdPin, RfidAuth, FaceAuth, FingerprintAuth,
                       EnrollRfid, EnrollFace, EnrollFingerprint, ConfirmOldDeleted };
inline bool textIs(const char *a, const char *b) { return std::strcmp(a,b)==0; }
inline bool cleanText(const char *s, bool allowEmpty=false) {
    if (!s || (!allowEmpty && !*s)) return false;
    for (; *s; ++s) if (static_cast<unsigned char>(*s)<32 ||
                        static_cast<unsigned char>(*s)>126) return false;
    return true;
}
inline bool flowValue(const char *s) {
    return textIs(s,"AUTHENTICATION") || textIs(s,"ENROLLMENT") || textIs(s,"REENROLLMENT");
}
inline bool credentialValue(const char *s) {
    return textIs(s,"RFID") || textIs(s,"FACE") || textIs(s,"FINGERPRINT");
}
inline bool nextValue(const char *s) {
    return credentialValue(s) || textIs(s,"DELETE_OLD_FINGERPRINTS") || textIs(s,"COMPLETE");
}
inline bool isInitial(Operation op) { return op==Operation::IdPin || op==Operation::RfidAuth; }
inline bool isAuthFollowup(Operation op) { return op==Operation::FaceAuth || op==Operation::FingerprintAuth; }
inline const cJSON *field(const cJSON *j, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(j,name);
}
inline bool stringField(const cJSON *j, const char *name, const char *&out) {
    const cJSON *v=field(j,name);
    if (!cJSON_IsString(v) || !cleanText(v->valuestring)) return false;
    out=v->valuestring; return true;
}
template<size_t N>
bool readText(const cJSON *j, const char *name, char (&out)[N], bool required=false) {
    const cJSON *v=field(j,name);
    if (!v) return !required;
    const char *s=nullptr;
    return stringField(j,name,s) && copyBackendText(out,s);
}
inline bool intValue(const cJSON *v, int lo, int hi, int &out) {
    if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) ||
        v->valuedouble<lo || v->valuedouble>hi ||
        std::floor(v->valuedouble)!=v->valuedouble) return false;
    out=static_cast<int>(v->valuedouble); return true;
}
inline bool optionalInt(const cJSON *j, const char *name, int lo, int hi, int &out) {
    const cJSON *v=field(j,name); return !v || cJSON_IsNull(v) || intValue(v,lo,hi,out);
}
inline bool unusedSession(const cJSON *v) {
    return !v || cJSON_IsNull(v) || (cJSON_IsString(v) && v->valuestring && !*v->valuestring);
}
// Limit nesting and reject embedded NULs or duplicate fields.
inline bool boundedDocument(const char *s, size_t n) {
    if (!s || !n || n>NetworkConfig::MAX_RESPONSE_BYTES || std::memchr(s,0,n)) return false;
    unsigned depth=0; bool quoted=false, escaped=false;
    for (size_t i=0;i<n;++i) {
        const unsigned char c=static_cast<unsigned char>(s[i]);
        if (quoted) {
            if (c<32) return false;
            if (escaped) { escaped=false; continue; }
            if (c=='\\') {
                if (i+5<n && s[i+1]=='u' && s[i+2]=='0' && s[i+3]=='0' &&
                    s[i+4]=='0' && s[i+5]=='0') return false;
                escaped=true;
            } else if (c=='"') quoted=false;
        } else if (c=='"') quoted=true;
        else if (c=='{' || c=='[') { if (++depth>8) return false; }
        else if (c=='}' || c==']') { if (!depth) return false; --depth; }
    }
    return !quoted && depth==0;
}
inline bool uniqueKeys(const cJSON *j) {
    for (const cJSON *a=j->child;a;a=a->next) {
        if (!a->string) return false;
        for (const cJSON *b=a->next;b;b=b->next)
            if (!b->string || textIs(a->string,b->string)) return false;
    }
    return true;
}
inline bool decodeObject(const cJSON *j, Operation op, const BackendRequestContext &ctx,
                         const char *wireId, int status, BackendResponse &r) {
    if (!cJSON_IsObject(j) || !uniqueKeys(j) || !ctx.requestId) return false;
    const char *echo=nullptr;
    if (!stringField(j,"request_id",echo) || !textIs(echo,wireId)) return false;
    const cJSON *success=field(j,"success");
    if (!cJSON_IsBool(success)) return false;
    r.requestId=ctx.requestId;
    if (!cJSON_IsTrue(success)) {
        if (field(j,"reason") && !readText(j,"reason",r.failureReason,true)) return false;
        if (!r.failureReason[0]) copyBackendText(r.failureReason,"Request rejected");
        return true;
    }
    if (status<200 || status>=300) return false;
    if (!isInitial(op)) {
        if (!flowValue(ctx.flow)) return false;
        copyBackendText(r.flow,ctx.flow);
        copyBackendText(r.credentialType,ctx.credentialType);
        copyBackendText(r.authSessionId,ctx.authSessionId);
        copyBackendText(r.enrollmentSessionId,ctx.enrollmentSessionId);
    }
    if (!readText(j,"flow",r.flow,isInitial(op)) || !flowValue(r.flow)) return false;
    if (ctx.flow[0] && !textIs(ctx.flow,r.flow)) return false;
    const bool auth=textIs(r.flow,"AUTHENTICATION");
    if ((op==Operation::RfidAuth || isAuthFollowup(op)) && !auth) return false;
    if (!isInitial(op) && !isAuthFollowup(op) && auth) return false;
    if (auth) {
        if (!readText(j,"session_id",r.authSessionId,isInitial(op)) || !r.authSessionId[0] ||
            !unusedSession(field(j,"enrollment_session_id"))) return false;
        if (ctx.authSessionId[0] && !textIs(ctx.authSessionId,r.authSessionId)) return false;
        r.enrollmentSessionId[0]=0;
    } else {
        if (!readText(j,"enrollment_session_id",r.enrollmentSessionId,isInitial(op)) ||
            !r.enrollmentSessionId[0] || !unusedSession(field(j,"session_id"))) return false;
        if (ctx.enrollmentSessionId[0] && !textIs(ctx.enrollmentSessionId,r.enrollmentSessionId)) return false;
        r.authSessionId[0]=0;
    }
    const cJSON *credential=field(j,"credential_type");
    if (credential) {
        // Accept an empty credential only when none is already bound.
        if (unusedSession(credential)) { if (ctx.credentialType[0]) return false; }
        else if (!readText(j,"credential_type",r.credentialType)) return false;
    }
    if (r.credentialType[0] && !credentialValue(r.credentialType)) return false;
    if (ctx.credentialType[0] && !textIs(ctx.credentialType,r.credentialType)) return false;
    if (textIs(r.flow,"REENROLLMENT") && !r.credentialType[0]) return false;

    if (!readText(j,"next_step",r.nextStep)) return false;
    const cJSON *legacy=field(j,"current_state");
    if (legacy && cJSON_IsString(legacy) && legacy->valuestring && textIs(legacy->valuestring,"COMPLETE")) {
        if (!auth || !isAuthFollowup(op)) return false;
        if (r.nextStep[0] && !textIs(r.nextStep,"COMPLETE")) return false;
        copyBackendText(r.nextStep,"COMPLETE");
    }
    if (!nextValue(r.nextStep)) return false;
    if (!optionalInt(j,"fingerprint_slot",BackendPolicy::FINGERPRINT_SLOT_MIN,
                     BackendPolicy::FINGERPRINT_SLOT_MAX,r.fingerprintSlot) ||
        !optionalInt(j,"fingerprints_enrolled",0,2,r.fingerprintsEnrolled) ||
        !optionalInt(j,"fingerprints_required",2,2,r.fingerprintsRequired) ||
        !optionalInt(j,"captures_completed",0,5,r.capturesCompleted) ||
        !optionalInt(j,"captures_required",5,5,r.capturesRequired) ||
        !optionalInt(j,"next_capture_index",1,5,r.nextCaptureIndex)) return false;
    const cJSON *prompt=field(j,"next_prompt");
    if (prompt && !cJSON_IsNull(prompt)) {
        if (cJSON_IsString(prompt) && prompt->valuestring && !*prompt->valuestring) {}
        else if (!readText(j,"next_prompt",r.nextPrompt)) return false;
    }
    const cJSON *slots=field(j,"old_slots");
    if (slots && !cJSON_IsNull(slots)) {
        if (!cJSON_IsArray(slots)) return false;
        int count=cJSON_GetArraySize(slots);
        if (count<0 || count>static_cast<int>(MAX_OLD_FINGERPRINT_SLOTS)) return false;
        r.hasOldFingerprintSlots=true; r.oldFingerprintSlotCount=static_cast<uint8_t>(count);
        for (int i=0;i<count;++i) {
            if (!intValue(cJSON_GetArrayItem(slots,i),BackendPolicy::FINGERPRINT_SLOT_MIN,
                          BackendPolicy::FINGERPRINT_SLOT_MAX,r.oldFingerprintSlots[i])) return false;
            for (int k=0;k<i;++k) if(r.oldFingerprintSlots[k]==r.oldFingerprintSlots[i]) return false;
        }
        int reported=count;
        if (!optionalInt(j,"old_slot_count",0,2,reported) || reported!=count) return false;
    } else if (field(j,"old_slot_count") && !cJSON_IsNull(field(j,"old_slot_count"))) return false;
    if (!auth && textIs(r.nextStep,"FINGERPRINT") && r.fingerprintSlot<0) return false;
    if (textIs(r.nextStep,"DELETE_OLD_FINGERPRINTS") &&
        (!textIs(r.flow,"REENROLLMENT") || !textIs(r.credentialType,"FINGERPRINT") ||
         !r.hasOldFingerprintSlots)) return false;
    r.success=true; return true;
}
inline bool decode(const char *body, size_t length, Operation op,
                   const BackendRequestContext &ctx, const char *wireId,
                   int status, BackendResponse &out) {
    out=BackendResponse{};
    if (!boundedDocument(body,length) || body[length]!='\0') return false;
    const char *end=nullptr;
    cJSON *j=cJSON_ParseWithOpts(body,&end,1);
    if (!j) return false;
    BackendResponse temporary{};
    bool ok=decodeObject(j,op,ctx,wireId,status,temporary);
    cJSON_Delete(j);
    if (ok) out=temporary; // Commit only a fully validated response.
    return ok;
}
}
