import asyncio
from contextlib import asynccontextmanager
import cv2
import numpy as np
from fastapi import FastAPI, UploadFile, File, Form, Depends, Header, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from insightface.app import FaceAnalysis
from pydantic import BaseModel, Field
import os
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from backend.auth_service import (
    authenticate_rfid,
    authenticate_pin,
    verify_face,
    verify_fingerprint,
    cleanup_expired_sessions
)
from backend.enrollment_service import (
    start_enrollment,
    enroll_rfid,
    enroll_face,
    enroll_fingerprint,
    start_face_reenrollment,
    start_rfid_reenrollment,
    start_fingerprint_reenrollment,
    confirm_old_fingerprints_deleted,
    cleanup_expired_enrollment_sessions,
    enrollment_sessions
)
from backend.admin_service import (
    create_pending_user,
    list_users,
    change_user_status,
    update_user,
    list_access_attempts,
    reset_credentials,
    authenticate_admin,
    verify_admin_token,
    change_admin_password
)
from backend.kiosk_service import handle_id_pin

async def session_cleanup_loop():  # Checks for expired sessions every 10 seconds 
    while True:
        cleanup_expired_sessions()
        cleanup_expired_enrollment_sessions()
        await asyncio.sleep(10)

@asynccontextmanager
async def lifespan(app: FastAPI):
    cleanup_task = asyncio.create_task(session_cleanup_loop())
    yield
    cleanup_task.cancel()
    try:
        await cleanup_task
    except asyncio.CancelledError:
        pass

app = FastAPI(lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=[
        "http://127.0.0.1:5500",
        "http://localhost:5500"
    ],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"]
)

def normalize_response(result, request_id, flow=None, session_id=None, enrollment_session_id=None, credential_type=None):
    response = dict(result)
    # Every ESP-facing response echoes the original request ID
    response["request_id"] = request_id
    if flow is not None:
        response["flow"] = flow
    if session_id is not None:
        response["session_id"] = session_id
    if enrollment_session_id is not None:
        response["enrollment_session_id"] = enrollment_session_id
    if credential_type is not None:
        response["credential_type"] = credential_type
    # ESP should consistently receive next_step = COMPLETE
    if response.get("success") is True and response.get("current_state") == "COMPLETE":
        response["next_step"] = "COMPLETE"
    return response

def get_enrollment_context(enrollment_session_id):
    session = enrollment_sessions.get(enrollment_session_id)
    if session is None:
        return None, None
    mode = session.get("mode", "NEW_ENROLLMENT")
    if mode == "NEW_ENROLLMENT":
        return "ENROLLMENT", None
    if mode == "FACE_REENROLLMENT":
        return "REENROLLMENT", "FACE"
    if mode == "RFID_REENROLLMENT":
        return "REENROLLMENT", "RFID"
    if mode == "FINGERPRINT_REENROLLMENT":
        return "REENROLLMENT", "FINGERPRINT"
    return None, None

DEVICE_API_KEY = os.getenv("DEVICE_API_KEY")
def require_device(x_device_key: str = Header(None)):
    if not DEVICE_API_KEY:
        raise HTTPException(
            status_code=500,
            detail="DEVICE_API_KEY_NOT_CONFIGURED"
        )
    if x_device_key != DEVICE_API_KEY:
        raise HTTPException(
            status_code=401,
            detail="INVALID_DEVICE_KEY"
        )
    return True

def require_admin(authorization: str = Header(None)):
    if authorization is None:
        raise HTTPException(status_code=401, detail="MISSING_TOKEN")
    if not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="INVALID_AUTH_HEADER")
    token = authorization[7:]
    result = verify_admin_token(token)
    if not result["success"]:
        raise HTTPException(status_code=401, detail=result["reason"])
    return result

class RFIDRequest(BaseModel):
    request_id: str
    rfid_uid: str

class PINRequest(BaseModel):
    request_id: str
    employee_id: str
    entered_pin: str

class KioskIDRequest(BaseModel):
    request_id: str
    employee_id: str
    pin: str

class FingerprintRequest(BaseModel):
    request_id: str
    session_id: str
    matched_template_slot: int | None

class EnrollmentStartRequest(BaseModel):
    request_id: str
    employee_id: str
    entered_pin: str

class EnrollmentRFIDRequest(BaseModel):
    request_id: str
    enrollment_session_id: str
    rfid_uid: str

class EnrollmentFingerprintRequest(BaseModel):
    request_id: str
    enrollment_session_id: str
    template_slot: int

class ConfirmFingerprintDeleteRequest(BaseModel):
    request_id: str
    enrollment_session_id: str

class CreateUserRequest(BaseModel):
    first_name: str
    last_name: str
    pin: str = Field(min_length=4, max_length=4, pattern=r"^\d{4}$")

class UserStatusRequest(BaseModel):
    status: str

class EditUserRequest(BaseModel):
    first_name: str
    last_name: str

class AdminLoginRequest(BaseModel):
    username: str
    password: str

class ChangeAdminPasswordRequest(BaseModel):
    current_password: str
    new_password: str = Field(min_length=8)

@app.get("/")
def root():
    return {
        "message": "Biometric Access API"
    }
@app.post("/admin/login")
def admin_login(request: AdminLoginRequest):
    return authenticate_admin(request.username, request.password)

# Main ID + PIN entry point used by the ESP
@app.post("/kiosk/id-pin")
def kiosk_id_pin(request: KioskIDRequest, device=Depends(require_device)):
    result = handle_id_pin(request.employee_id, request.pin)
    flow = result.get("flow")
    credential_type = result.get("credential_type")
    session_id = result.get("session_id")
    enrollment_session_id = result.get("enrollment_session_id")
    return normalize_response(
        result=result,
        request_id=request.request_id,
        flow=flow,
        session_id=session_id,
        enrollment_session_id=enrollment_session_id,
        credential_type=credential_type
    )

@app.post("/auth/rfid")
def auth_rfid(request: RFIDRequest, device=Depends(require_device)):
    result = authenticate_rfid(request.rfid_uid)
    session_id = result.get("session_id")
    return normalize_response(
        result=result,
        request_id=request.request_id,
        flow="AUTHENTICATION",
        session_id=session_id,
        credential_type=result.get("credential_type")
    )

# Development endpoint. ESP normally uses /kiosk/id-pin
@app.post("/auth/pin")
def auth_pin(request: PINRequest, device=Depends(require_device)):
    result = authenticate_pin(request.employee_id, request.entered_pin)
    session_id = result.get("session_id")
    return normalize_response(
        result=result,
        request_id=request.request_id,
        flow="AUTHENTICATION",
        session_id=session_id,
        credential_type=result.get("credential_type")
    )

face_app = FaceAnalysis(name="buffalo_l")
face_app.prepare(ctx_id=-1)

@app.post("/auth/face")
async def auth_face( request_id: str = Form(...), session_id: str = Form(...), image: UploadFile = File(...), device=Depends(require_device)):
    image_bytes = await image.read()
    image_array = np.frombuffer(image_bytes, dtype=np.uint8)
    frame = cv2.imdecode(image_array, cv2.IMREAD_COLOR)
    if frame is None:
        return normalize_response(
            result={
                "success": False,
                "reason": "INVALID_IMAGE"
            },
            request_id=request_id,
            flow="AUTHENTICATION",
            session_id=session_id
        )
    faces = face_app.get(frame)
    if len(faces) == 0:
        return normalize_response(
            result={
                "success": False,
                "reason": "NO_FACE_DETECTED"
            },
            request_id=request_id,
            flow="AUTHENTICATION",
            session_id=session_id
        )
    if len(faces) > 1:
        return normalize_response(
            result={
                "success": False,
                "reason": "MULTIPLE_FACES_DETECTED"
            },
            request_id=request_id,
            flow="AUTHENTICATION",
            session_id=session_id
        )
    live_embedding = faces[0].normed_embedding
    result = verify_face(session_id, live_embedding)
    return normalize_response(
        result=result,
        request_id=request_id,
        flow="AUTHENTICATION",
        session_id=session_id,
        credential_type=result.get("credential_type")
    )

@app.post("/auth/fingerprint")
def auth_fingerprint(request: FingerprintRequest, device=Depends(require_device)):
    result = verify_fingerprint(request.session_id, request.matched_template_slot)
    return normalize_response(
        result=result,
        request_id=request.request_id,
        flow="AUTHENTICATION",
        session_id=request.session_id,
        credential_type=result.get("credential_type")
    )

# Development endpoint. ESP normally uses /kiosk/id-pin
@app.post("/enroll/start")
def enroll_start(request: EnrollmentStartRequest, device=Depends(require_device)):
    result = start_enrollment(request.employee_id, request.entered_pin)
    enrollment_session_id = result.get("enrollment_session_id")
    return normalize_response(
        result=result,
        request_id=request.request_id,
        flow="ENROLLMENT",
        enrollment_session_id=enrollment_session_id
    )

@app.post("/enroll/rfid")
def enroll_rfid_endpoint(request: EnrollmentRFIDRequest, device=Depends(require_device)):
    flow, credential_type = get_enrollment_context(request.enrollment_session_id)
    result = enroll_rfid(request.enrollment_session_id, request.rfid_uid)
    return normalize_response(
        result=result,
        request_id=request.request_id,
        flow=flow,
        enrollment_session_id=request.enrollment_session_id,
        credential_type=credential_type
    )

@app.post("/enroll/face")
async def enroll_face_endpoint(
    request_id: str = Form(...),
    enrollment_session_id: str = Form(...),
    image: UploadFile = File(...),
    device=Depends(require_device)
):
    flow, credential_type = get_enrollment_context(enrollment_session_id)
    image_bytes = await image.read()
    image_array = np.frombuffer(image_bytes, dtype=np.uint8)
    frame = cv2.imdecode(image_array, cv2.IMREAD_COLOR)
    if frame is None:
        return normalize_response(
            result={
                "success": False,
                "reason": "INVALID_IMAGE",
                "retry": True
            },
            request_id=request_id,
            flow=flow,
            enrollment_session_id=enrollment_session_id,
            credential_type=credential_type
        )
    faces = face_app.get(frame)
    if len(faces) == 0:
        return normalize_response(
            result={
                "success": False,
                "reason": "NO_FACE_DETECTED",
                "retry": True
            },
            request_id=request_id,
            flow=flow,
            enrollment_session_id=enrollment_session_id,
            credential_type=credential_type
        )
    if len(faces) > 1:
        return normalize_response(
            result={
                "success": False,
                "reason": "MULTIPLE_FACES_DETECTED",
                "retry": True
            },
            request_id=request_id,
            flow=flow,
            enrollment_session_id=enrollment_session_id,
            credential_type=credential_type
        )
    face_embedding = faces[0].normed_embedding
    result = enroll_face(enrollment_session_id, face_embedding)
    return normalize_response(
        result=result,
        request_id=request_id,
        flow=flow,
        enrollment_session_id=enrollment_session_id,
        credential_type=credential_type
    )

@app.post("/enroll/fingerprint")
def enroll_fingerprint_endpoint(request: EnrollmentFingerprintRequest, device=Depends(require_device)):
    flow, credential_type = get_enrollment_context(request.enrollment_session_id)
    result = enroll_fingerprint(request.enrollment_session_id, request.template_slot)
    return normalize_response(
        result=result,
        request_id=request.request_id,
        flow=flow,
        enrollment_session_id=request.enrollment_session_id,
        credential_type=credential_type
    )

# Called only after ESP successfully deletes the old fingerprnt templates
@app.post("/enroll/fingerprint/confirm-old-deleted")
def confirm_old_fingerprints_deleted_endpoint(request: ConfirmFingerprintDeleteRequest, device=Depends(require_device)):
    flow, credential_type = get_enrollment_context(request.enrollment_session_id)
    result = confirm_old_fingerprints_deleted(request.enrollment_session_id)
    return normalize_response(
        result=result,
        request_id=request.request_id,
        flow=flow,
        enrollment_session_id=request.enrollment_session_id,
        credential_type=credential_type
    )

@app.post("/admin/users")
def create_user(request: CreateUserRequest, admin=Depends(require_admin)):
    return create_pending_user(request.pin, request.first_name, request.last_name)

@app.get("/admin/users")
def get_users(admin=Depends(require_admin)):
    return list_users()

@app.patch("/admin/users/{user_id}/status")
def update_user_status(user_id: int, request: UserStatusRequest, admin=Depends(require_admin)):
    return change_user_status(user_id, request.status)

@app.patch("/admin/users/{user_id}")
def edit_user(user_id: int, request: EditUserRequest, admin=Depends(require_admin)):
    return update_user(user_id, request.first_name, request.last_name)

@app.get("/admin/access-attempts")
def get_access_attempts_endpoint(admin=Depends(require_admin)):
    return list_access_attempts()

@app.post("/admin/users/{user_id}/reset-credentials")
def reset_credentials_endpoint(user_id: int, admin=Depends(require_admin)):
    return reset_credentials(user_id)

@app.post("/admin/users/{user_id}/face/reenroll")
def start_face_reenrollment_endpoint(user_id: int, admin=Depends(require_admin)):
    return start_face_reenrollment(user_id)

@app.post("/admin/users/{user_id}/rfid/reenroll")
def start_rfid_reenrollment_endpoint(user_id: int, admin=Depends(require_admin)):
    return start_rfid_reenrollment(user_id)

@app.post("/admin/users/{user_id}/fingerprints/reenroll")
def start_fingerprint_reenrollment_endpoint(user_id: int, admin=Depends(require_admin)):
    return start_fingerprint_reenrollment(user_id)

@app.patch("/admin/password")
def update_admin_password(request: ChangeAdminPasswordRequest, admin=Depends(require_admin)):
    return change_admin_password(admin["admin_id"], request.current_password, request.new_password)

app.mount("/dashboard", StaticFiles(directory="dashboard"), name="dashboard")
@app.get("/admin-dashboard")
def admin_dashboard():
    return FileResponse("dashboard/login.html")