import random
import uuid
import time
import json
import numpy as np
from argon2 import PasswordHasher
from argon2.exceptions import VerifyMismatchError
from backend.repositories import (
    find_user_by_rfid,
    find_user_by_employee_id,
    get_pin_hash,
    get_fingerprint_template,
    get_face_embeddings,
    get_pending_reenrollment,
    create_access_attempt,
    finish_access_attempt
)
ph = PasswordHasher()
sessions = {}

def debug_sessions():  # Used to see how many authentication sessions are in RAM
    print("\nCurrent Sessions:")
    print(sessions)
    print("Session Count:", len(sessions))

def cleanup_expired_sessions():  # Removes expired authentication sessions from RAM
    current_time = time.time()
    expired_session_ids = []
    for session_id, session in sessions.items():
        expires_at = session.get("expires_at")
        if expires_at is not None and expires_at < current_time:
            expired_session_ids.append(session_id)
    for session_id in expired_session_ids:
        session = sessions[session_id]
        if session.get("current_state") == "WAITING_FOR_BIOMETRIC":
            attempt_id = session.get("attempt_id")
            if attempt_id is not None:
                finish_access_attempt(attempt_id, session.get("next_step"), "FAIL", "EXPIRED", "SESSION_EXPIRED")
        del sessions[session_id]
    return len(expired_session_ids)

def check_pending_reenrollment(user_id):  # Checks whether the user has a pending re-enrollment
    pending = get_pending_reenrollment(user_id)
    if pending is None:
        return None
    if isinstance(pending, (tuple, list)):
        if len(pending) == 0:
            return None
        return pending[0]
    return pending

def create_session(user_id, next_step):
    session_id = str(uuid.uuid4())
    sessions[session_id] = {
        "user_id": user_id,
        "next_step": next_step,
        "current_state": "WAITING_FOR_BIOMETRIC",
        "expires_at": time.time() + 30
    }
    return session_id

def authenticate_rfid(rfid_uid):
    cleanup_expired_sessions()
    user = find_user_by_rfid(rfid_uid)
    if user is None:
        attempt_id = create_access_attempt(None, "RFID", "FAIL")
        finish_access_attempt(attempt_id, None, None, "FAIL", "UNKNOWN_RFID")
        return {
            "success": False,
            "reason": "UNKNOWN_RFID"
        }
    user_id = user[0]
    status = user[4]
    if status != "ACTIVE":
        attempt_id = create_access_attempt(user_id, "RFID", "FAIL")
        finish_access_attempt(attempt_id, None, None, "FAIL", "USER_INACTIVE")
        return {
            "success": False,
            "reason": "USER_INACTIVE"
        }
    # Block normal authentication while credential re-enrollment is pending
    pending_reenrollment = check_pending_reenrollment(user_id)
    if pending_reenrollment is not None:
        attempt_id = create_access_attempt(user_id, "RFID", "FAIL")
        finish_access_attempt(attempt_id, None, None, "FAIL", "REENROLLMENT_REQUIRED")
        return {
            "success": False,
            "reason": "REENROLLMENT_REQUIRED",
            "credential_type": pending_reenrollment
        }
    # Randomly choose the second authentication factor
    next_step = random.choice(["FACE", "FINGERPRINT"])
    session_id = create_session(user_id, next_step)
    attempt_id = create_access_attempt(user_id, "RFID", "SUCCESS")
    sessions[session_id]["attempt_id"] = attempt_id
    return {
        "success": True,
        "user_id": user_id,
        "next_step": next_step,
        "session_id": session_id
    }

def authenticate_pin(employee_id, entered_pin):
    cleanup_expired_sessions()
    user = find_user_by_employee_id(employee_id)
    if user is None:
        attempt_id = create_access_attempt(None, "PIN", "FAIL")
        finish_access_attempt(attempt_id, None, None, "FAIL", "UNKNOWN_USER")
        return {
            "success": False,
            "reason": "UNKNOWN_USER"
        }
    user_id = user[0]
    status = user[4]
    if status != "ACTIVE":
        attempt_id = create_access_attempt(user_id, "PIN", "FAIL")
        finish_access_attempt(attempt_id, None, None, "FAIL", "USER_INACTIVE")
        return {
            "success": False,
            "reason": "USER_INACTIVE"
        }
    # Block normal authentication while credential re-enrollment is pending
    pending_reenrollment = check_pending_reenrollment(user_id)
    if pending_reenrollment is not None:
        attempt_id = create_access_attempt(user_id, "PIN", "FAIL")
        finish_access_attempt(attempt_id, None, None, "FAIL", "REENROLLMENT_REQUIRED")
        return {
            "success": False,
            "reason": "REENROLLMENT_REQUIRED",
            "credential_type": pending_reenrollment
        }
    pin = get_pin_hash(user_id)
    if pin is None:
        attempt_id = create_access_attempt(user_id, "PIN", "FAIL")
        finish_access_attempt(attempt_id, None, None, "FAIL", "UNKNOWN_PIN")
        return {
            "success": False,
            "reason": "UNKNOWN_PIN"
        }
    try:
        ph.verify(pin[0], entered_pin)
    except VerifyMismatchError:
        attempt_id = create_access_attempt(user_id, "PIN", "FAIL")
        finish_access_attempt(attempt_id, None, None, "FAIL", "INVALID_PIN")
        return {
            "success": False,
            "reason": "INVALID_PIN"
        }
    # Randomly choose the second authentication factor
    next_step = random.choice(["FACE", "FINGERPRINT"])
    session_id = create_session(user_id, next_step)
    attempt_id = create_access_attempt(user_id, "PIN", "SUCCESS")
    sessions[session_id]["attempt_id"] = attempt_id
    return {
        "success": True,
        "user_id": user_id,
        "next_step": next_step,
        "session_id": session_id
    }

# # Used by kiosk routing before deciding between authentication or re-enrollment.
def verify_pin_only(employee_id, entered_pin):
    user = find_user_by_employee_id(employee_id)
    if user is None:
        return {
            "success": False,
            "reason": "UNKNOWN_USER"
        }
    user_id = user[0]
    pin = get_pin_hash(user_id)
    if pin is None:
        return {
            "success": False,
            "reason": "UNKNOWN_PIN"
        }
    try:
        ph.verify(pin[0], entered_pin)
    except VerifyMismatchError:
        return {
            "success": False,
            "reason": "INVALID_PIN"
        }
    return {
        "success": True,
        "user_id": user_id
    }

def verify_fingerprint(session_id, matched_template_slot):
    cleanup_expired_sessions()
    if session_id not in sessions:
        return {
            "success": False,
            "reason": "UNKNOWN_SESSION_ID"
        }
    session = sessions[session_id]
    if session["current_state"] != "WAITING_FOR_BIOMETRIC":
        return {
            "success": False,
            "reason": "NOT_IN_WAITING_STATE"
        }
    if session["next_step"] != "FINGERPRINT":
        finish_access_attempt(session["attempt_id"], "FINGERPRINT", "FAIL", "FAIL", "WRONG_BIOMETRIC_CHECK")
        del sessions[session_id]
        return {
            "success": False,
            "reason": "WRONG_BIOMETRIC_CHECK"
        }
    user_id = session["user_id"]
    # Re-check in case an admin requested re-enrollment after factor one succeeded
    pending_reenrollment = check_pending_reenrollment(user_id)
    if pending_reenrollment is not None:
        finish_access_attempt(session["attempt_id"], "FINGERPRINT", "FAIL", "FAIL", "REENROLLMENT_REQUIRED")
        del sessions[session_id]
        return {
            "success": False,
            "reason": "REENROLLMENT_REQUIRED",
            "credential_type": pending_reenrollment
        }
    user_templates = get_fingerprint_template(user_id)
    if not user_templates:
        finish_access_attempt(session["attempt_id"], "FINGERPRINT", "FAIL", "FAIL", "NO_ENROLLED_FINGERPRINT")
        del sessions[session_id]
        return {
            "success": False,
            "reason": "NO_ENROLLED_FINGERPRINT"
        }
    if matched_template_slot is None:
        finish_access_attempt(session["attempt_id"], "FINGERPRINT", "FAIL", "FAIL", "FINGERPRINT_MISMATCH")
        del sessions[session_id]
        return {
            "success": False,
            "reason": "FINGERPRINT_MISMATCH"
        }
    for template in user_templates:
        if template[0] == matched_template_slot:
            finish_access_attempt(session["attempt_id"], "FINGERPRINT", "SUCCESS", "SUCCESS", None)
            del sessions[session_id]
            return {
                "success": True,
                "user_id": user_id,
                "current_state": "COMPLETE"
            }
    finish_access_attempt(session["attempt_id"], "FINGERPRINT", "FAIL", "FAIL", "FINGERPRINT_MISMATCH")
    del sessions[session_id]
    return {
        "success": False,
        "reason": "FINGERPRINT_MISMATCH"
    }

def verify_face(session_id, live_embedding):
    cleanup_expired_sessions()
    FACE_THRESHOLD = 0.95  # Maximum embedding distance accepted as a face match
    if session_id not in sessions:
        return {
            "success": False,
            "reason": "UNKNOWN_SESSION_ID"
        }
    session = sessions[session_id]
    if session["current_state"] != "WAITING_FOR_BIOMETRIC":
        return {
            "success": False,
            "reason": "NOT_IN_WAITING_STATE"
        }
    if session["next_step"] != "FACE":
        finish_access_attempt(session["attempt_id"], "FACE", "FAIL", "FAIL", "WRONG_BIOMETRIC_CHECK")
        del sessions[session_id]
        return {
            "success": False,
            "reason": "WRONG_BIOMETRIC_CHECK"
        }
    user_id = session["user_id"]
    # Re-check in case an admin requested re-enrollment after factor one succeeded
    pending_reenrollment = check_pending_reenrollment(user_id)
    if pending_reenrollment is not None:
        finish_access_attempt(session["attempt_id"], "FACE", "FAIL", "FAIL", "REENROLLMENT_REQUIRED")
        del sessions[session_id]
        return {
            "success": False,
            "reason": "REENROLLMENT_REQUIRED",
            "credential_type": pending_reenrollment
        }
    stored_face = get_face_embeddings(user_id)
    if not stored_face:
        finish_access_attempt(session["attempt_id"], "FACE", "FAIL", "FAIL", "NO_ENROLLED_FACE")
        del sessions[session_id]
        return {
            "success": False,
            "reason": "NO_ENROLLED_FACE"
        }
    live_embedding = np.array(live_embedding, dtype=np.float32)
    stored_embedding = np.array(json.loads(stored_face[0]), dtype=np.float32)
    distance = np.linalg.norm(live_embedding - stored_embedding)
    if distance <= FACE_THRESHOLD:
        finish_access_attempt(session["attempt_id"], "FACE", "SUCCESS", "SUCCESS", None)
        del sessions[session_id]
        return {
            "success": True,
            "user_id": user_id,
            "distance": float(distance),
            "threshold": FACE_THRESHOLD,
            "current_state": "COMPLETE"
        }
    finish_access_attempt(session["attempt_id"], "FACE", "FAIL", "FAIL", "FACE_MISMATCH")
    del sessions[session_id]
    return {
        "success": False,
        "reason": "FACE_MISMATCH",
        "distance": float(distance),
        "threshold": FACE_THRESHOLD
    }