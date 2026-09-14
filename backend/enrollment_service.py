import time
import uuid
import json
import numpy as np
from argon2 import PasswordHasher
from argon2.exceptions import VerifyMismatchError
from backend.repositories import (
    find_user_by_employee_id,
    find_user_by_id,
    find_user_by_rfid,
    get_pin_hash,
    get_fingerprint_template,
    get_next_free_fingerprint_slot,
    fingerprint_slot_exists,
    add_rfid_credential,
    add_face_credential,
    add_fingerprint_credential,
    replace_face_credential,
    replace_rfid_credential,
    delete_specific_fingerprint_credentials,
    set_user_status,
    set_pending_reenrollment,
    clear_pending_reenrollment
)
ph = PasswordHasher()
enrollment_sessions = {}
FACE_PROMPTS = [
    "LOOK_STRAIGHT",
    "TURN_SLIGHTLY_LEFT",
    "TURN_MORE_LEFT",
    "TURN_SLIGHTLY_RIGHT",
    "TURN_MORE_RIGHT"
]

def cleanup_expired_enrollment_sessions():
    current_time = time.time()
    expired_session_ids = []
    for session_id, session in enrollment_sessions.items():
        expires_at = session.get("expires_at")
        if expires_at is not None and expires_at < current_time:
            expired_session_ids.append(session_id)
    for session_id in expired_session_ids:
        session = enrollment_sessions[session_id]
        user_id = session.get("user_id")
        mode = session.get("mode")
        # Keep old fingerprints during re-enrollment  Remove only temporary replacement DB mappings if the session expires
        if mode == "FINGERPRINT_REENROLLMENT":
            new_slots = session.get("new_slots", [])
            if user_id is not None and len(new_slots) > 0:
                delete_specific_fingerprint_credentials(user_id, new_slots)
        del enrollment_sessions[session_id]
    return len(expired_session_ids)

def allocate_fingerprint_slot(session):
    slot = get_next_free_fingerprint_slot()
    if slot is None:
        return None
    session["expected_fingerprint_slot"] = slot
    return slot

def start_enrollment(employee_id, entered_pin):
    cleanup_expired_enrollment_sessions()
    user = find_user_by_employee_id(employee_id)
    if user is None:
        return {
            "success": False,
            "reason": "UNKNOWN_USER"
        }
    user_id = user[0]
    status = user[4]
    if status != "PENDING_ENROLLMENT":
        return {
            "success": False,
            "reason": "NOT_PENDING_ENROLLMENT"
        }
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
    enrollment_session_id = str(uuid.uuid4())
    enrollment_sessions[enrollment_session_id] = {
        "user_id": user_id,
        "current_step": "RFID",
        "mode": "NEW_ENROLLMENT",
        "expires_at": time.time() + 300
    }
    return {
        "success": True,
        "user_id": user_id,
        "enrollment_session_id": enrollment_session_id,
        "current_step": "RFID",
        "next_step": "RFID"
    }

def enroll_rfid(enrollment_session_id, rfid_uid):
    cleanup_expired_enrollment_sessions()
    if enrollment_session_id not in enrollment_sessions:
        return {
            "success": False,
            "reason": "UNKNOWN_ENROLLMENT_SESSION"
        }
    session = enrollment_sessions[enrollment_session_id]
    if session["current_step"] != "RFID":
        return {
            "success": False,
            "reason": "WRONG_ENROLLMENT_STEP"
        }
    user_id = session["user_id"]
    mode = session.get("mode", "NEW_ENROLLMENT")
    existing_rfid = find_user_by_rfid(rfid_uid)
    if existing_rfid is not None:
        existing_user_id = existing_rfid[0]
        if existing_user_id != user_id:
            return {
                "success": False,
                "reason": "RFID_ALREADY_REGISTERED"
            }
        if mode == "RFID_REENROLLMENT":
            return {
                "success": False,
                "reason": "SAME_RFID"
            }
    if mode == "RFID_REENROLLMENT":
        replace_rfid_credential(user_id, rfid_uid)
        clear_pending_reenrollment(user_id)
        del enrollment_sessions[enrollment_session_id]
        return {
            "success": True,
            "user_id": user_id,
            "current_step": "COMPLETE",
            "next_step": "COMPLETE"
        }
    add_rfid_credential(user_id, rfid_uid)
    session["current_step"] = "FACE"
    return {
        "success": True,
        "user_id": user_id,
        "current_step": "FACE",
        "next_step": "FACE",
        "next_prompt": FACE_PROMPTS[0]
    }

def enroll_face(enrollment_session_id, face_embedding):
    cleanup_expired_enrollment_sessions()
    if enrollment_session_id not in enrollment_sessions:
        return {
            "success": False,
            "reason": "UNKNOWN_ENROLLMENT_SESSION"
        }
    session = enrollment_sessions[enrollment_session_id]
    if session["current_step"] != "FACE":
        return {
            "success": False,
            "reason": "WRONG_ENROLLMENT_STEP"
        }
    if "face_embeddings" not in session:
        session["face_embeddings"] = []
    face_embedding = np.array(face_embedding, dtype=np.float32)
    session["face_embeddings"].append(face_embedding)
    captures_completed = len(session["face_embeddings"])
    if captures_completed < 5:
        return {
            "success": True,
            "current_step": "FACE",
            "next_step": "FACE",
            "captures_completed": captures_completed,
            "captures_required": 5,
            "next_capture_index": captures_completed + 1,
            "next_prompt": FACE_PROMPTS[captures_completed]
        }
    # Average all face captures and normalize the final embedding
    embeddings = np.array(session["face_embeddings"])
    average_embedding = np.mean(embeddings, axis=0)
    norm = np.linalg.norm(average_embedding)
    if norm == 0:
        return {
            "success": False,
            "reason": "INVALID_FACE_EMBEDDING"
        }
    average_embedding = average_embedding / norm
    embedding_json = json.dumps(average_embedding.tolist())
    user_id = session["user_id"]
    mode = session.get("mode", "NEW_ENROLLMENT")
    if mode == "FACE_REENROLLMENT":
        replace_face_credential(user_id, embedding_json)
        clear_pending_reenrollment(user_id)
        del enrollment_sessions[enrollment_session_id]
        return {
            "success": True,
            "user_id": user_id,
            "captures_completed": 5,
            "current_step": "COMPLETE",
            "next_step": "COMPLETE"
        }
    fingerprint_slot = allocate_fingerprint_slot(session)
    if fingerprint_slot is None:
        return {
            "success": False,
            "reason": "NO_FINGERPRINT_SLOTS_AVAILABLE"
        }
    add_face_credential(user_id, embedding_json)
    session.pop("face_embeddings", None)
    session["current_step"] = "FINGERPRINT"
    return {
        "success": True,
        "user_id": user_id,
        "captures_completed": 5,
        "current_step": "FINGERPRINT",
        "next_step": "FINGERPRINT",
        "fingerprint_slot": fingerprint_slot,
        "fingerprints_enrolled": 0,
        "fingerprints_required": 2
    }

def enroll_fingerprint(enrollment_session_id, template_slot):
    cleanup_expired_enrollment_sessions()
    if enrollment_session_id not in enrollment_sessions:
        return {
            "success": False,
            "reason": "UNKNOWN_ENROLLMENT_SESSION"
        }
    session = enrollment_sessions[enrollment_session_id]
    if session["current_step"] != "FINGERPRINT":
        return {
            "success": False,
            "reason": "WRONG_ENROLLMENT_STEP"
        }
    user_id = session["user_id"]
    mode = session.get("mode", "NEW_ENROLLMENT")
    # ESP must store the fingerprint in the slot assigned by the backend
    expected_slot = session.get("expected_fingerprint_slot")
    if expected_slot is None:
        return {
            "success": False,
            "reason": "NO_FINGERPRINT_SLOT_ASSIGNED"
        }
    if template_slot != expected_slot:
        return {
            "success": False,
            "reason": "UNEXPECTED_FINGERPRINT_SLOT",
            "expected_slot": expected_slot
        }
    if fingerprint_slot_exists(template_slot):
        return {
            "success": False,
            "reason": "TEMPLATE_SLOT_ALREADY_USED"
        }
    # Keep old fingerprints until both replacements are enrolled successfully
    if mode == "FINGERPRINT_REENROLLMENT":
        add_fingerprint_credential(user_id, template_slot)
        session["new_slots"].append(template_slot)
        session.pop("expected_fingerprint_slot", None)
        replacements_enrolled = len(session["new_slots"])
        if replacements_enrolled < 2:
            next_fingerprint_slot = allocate_fingerprint_slot(session)
            if next_fingerprint_slot is None:
                return {
                    "success": False,
                    "reason": "NO_FINGERPRINT_SLOTS_AVAILABLE"
                }
            return {
                "success": True,
                "user_id": user_id,
                "current_step": "FINGERPRINT",
                "next_step": "FINGERPRINT",
                "fingerprints_enrolled": replacements_enrolled,
                "fingerprints_required": 2,
                "fingerprint_slot": next_fingerprint_slot
            }
        # ESP deletes the old physical templates only after both replacements exist
        session["current_step"] = "DELETE_OLD_FINGERPRINTS"
        old_slots = session.get("old_slots", [])
        return {
            "success": True,
            "user_id": user_id,
            "current_step": "DELETE_OLD_FINGERPRINTS",
            "next_step": "DELETE_OLD_FINGERPRINTS",
            "old_slots": old_slots,
            "old_slot_count": len(old_slots),
            "new_slots": session["new_slots"]
        }
    user_templates = get_fingerprint_template(user_id)
    if len(user_templates) >= 2:
        return {
            "success": False,
            "reason": "FINGERPRINTS_ALREADY_ENROLLED"
        }
    add_fingerprint_credential(user_id, template_slot)
    session.pop("expected_fingerprint_slot", None)
    user_templates = get_fingerprint_template(user_id)
    fingerprints_enrolled = len(user_templates)
    if fingerprints_enrolled < 2:
        next_fingerprint_slot = allocate_fingerprint_slot(session)
        if next_fingerprint_slot is None:
            return {
                "success": False,
                "reason": "NO_FINGERPRINT_SLOTS_AVAILABLE"
            }
        return {
            "success": True,
            "user_id": user_id,
            "current_step": "FINGERPRINT",
            "next_step": "FINGERPRINT",
            "fingerprints_enrolled": fingerprints_enrolled,
            "fingerprints_required": 2,
            "fingerprint_slot": next_fingerprint_slot
        }
    set_user_status(user_id, "ACTIVE")
    del enrollment_sessions[enrollment_session_id]
    return {
        "success": True,
        "user_id": user_id,
        "fingerprints_enrolled": 2,
        "current_step": "COMPLETE",
        "next_step": "COMPLETE"
    }

def confirm_old_fingerprints_deleted(enrollment_session_id):
    cleanup_expired_enrollment_sessions()
    if enrollment_session_id not in enrollment_sessions:
        return {
            "success": False,
            "reason": "UNKNOWN_ENROLLMENT_SESSION"
        }
    session = enrollment_sessions[enrollment_session_id]
    if session.get("mode") != "FINGERPRINT_REENROLLMENT":
        return {
            "success": False,
            "reason": "NOT_FINGERPRINT_REENROLLMENT"
        }
    if session["current_step"] != "DELETE_OLD_FINGERPRINTS":
        return {
            "success": False,
            "reason": "WRONG_ENROLLMENT_STEP"
        }
    user_id = session["user_id"]
    old_slots = session.get("old_slots", [])
    new_slots = session.get("new_slots", [])
    if len(new_slots) != 2:
        return {
            "success": False,
            "reason": "REPLACEMENT_FINGERPRINTS_INCOMPLETE"
        }
    delete_specific_fingerprint_credentials(user_id, old_slots)
    clear_pending_reenrollment(user_id)
    del enrollment_sessions[enrollment_session_id]
    return {
        "success": True,
        "user_id": user_id,
        "fingerprints_enrolled": 2,
        "new_slots": new_slots,
        "current_step": "COMPLETE",
        "next_step": "COMPLETE"
    }

def start_face_reenrollment(user_id):
    user = find_user_by_id(user_id)
    if user is None:
        return {
            "success": False,
            "reason": "UNKNOWN_USER"
        }
    if user[4] == "PENDING_ENROLLMENT":
        return {
            "success": False,
            "reason": "USER_NOT_FULLY_ENROLLED"
        }
    set_pending_reenrollment(user_id, "FACE")
    return {
        "success": True,
        "user_id": user_id,
        "pending_reenrollment": "FACE"
    }

def start_rfid_reenrollment(user_id):
    user = find_user_by_id(user_id)
    if user is None:
        return {
            "success": False,
            "reason": "UNKNOWN_USER"
        }
    if user[4] == "PENDING_ENROLLMENT":
        return {
            "success": False,
            "reason": "USER_NOT_FULLY_ENROLLED"
        }
    set_pending_reenrollment(user_id, "RFID")
    return {
        "success": True,
        "user_id": user_id,
        "pending_reenrollment": "RFID"
    }


def start_fingerprint_reenrollment(user_id):
    user = find_user_by_id(user_id)
    if user is None:
        return {
            "success": False,
            "reason": "UNKNOWN_USER"
        }
    if user[4] == "PENDING_ENROLLMENT":
        return {
            "success": False,
            "reason": "USER_NOT_FULLY_ENROLLED"
        }
    set_pending_reenrollment(user_id, "FINGERPRINT")
    return {
        "success": True,
        "user_id": user_id,
        "pending_reenrollment": "FINGERPRINT"
    }

def create_face_reenrollment_session(user_id):
    cleanup_expired_enrollment_sessions()
    session_id = str(uuid.uuid4())
    enrollment_sessions[session_id] = {
        "user_id": user_id,
        "current_step": "FACE",
        "mode": "FACE_REENROLLMENT",
        "expires_at": time.time() + 300
    }
    return {
        "success": True,
        "enrollment_session_id": session_id,
        "current_step": "FACE",
        "next_step": "FACE",
        "next_prompt": FACE_PROMPTS[0]
    }


def create_rfid_reenrollment_session(user_id):
    cleanup_expired_enrollment_sessions()
    session_id = str(uuid.uuid4())
    enrollment_sessions[session_id] = {
        "user_id": user_id,
        "current_step": "RFID",
        "mode": "RFID_REENROLLMENT",
        "expires_at": time.time() + 300
    }
    return {
        "success": True,
        "enrollment_session_id": session_id,
        "current_step": "RFID",
        "next_step": "RFID"
    }


def create_fingerprint_reenrollment_session(user_id):
    cleanup_expired_enrollment_sessions()
    old_templates = get_fingerprint_template(user_id)
    old_slots = [template[0] for template in old_templates]
    if len(old_slots) == 0:
        return {
            "success": False,
            "reason": "NO_EXISTING_FINGERPRINTS"
        }
    session_id = str(uuid.uuid4())
    enrollment_sessions[session_id] = {
        "user_id": user_id,
        "current_step": "FINGERPRINT",
        "mode": "FINGERPRINT_REENROLLMENT",
        "old_slots": old_slots,
        "new_slots": [],
        "expires_at": time.time() + 300
    }
    fingerprint_slot = allocate_fingerprint_slot(enrollment_sessions[session_id])
    if fingerprint_slot is None:
        del enrollment_sessions[session_id]
        return {
            "success": False,
            "reason": "NO_FINGERPRINT_SLOTS_AVAILABLE"
        }
    return {
        "success": True,
        "user_id": user_id,
        "enrollment_session_id": session_id,
        "current_step": "FINGERPRINT",
        "next_step": "FINGERPRINT",
        "fingerprint_slot": fingerprint_slot,
        "fingerprints_enrolled": 0,
        "fingerprints_required": 2
    }