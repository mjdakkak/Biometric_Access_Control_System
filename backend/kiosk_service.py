from backend.repositories import (
    find_user_by_employee_id,
    get_pending_reenrollment
)
from backend.auth_service import (
    authenticate_pin,
    verify_pin_only
)
from backend.enrollment_service import (
    start_enrollment,
    create_face_reenrollment_session,
    create_rfid_reenrollment_session,
    create_fingerprint_reenrollment_session
)

def handle_id_pin(employee_id, entered_pin):
    user = find_user_by_employee_id(employee_id)
    if user is None:
        return {
            "success": False,
            "reason": "UNKNOWN_USER"
        }
    user_id = user[0]
    status = user[4]
    if status == "PENDING_ENROLLMENT":
        result = start_enrollment(employee_id, entered_pin)
        result["flow"] = "ENROLLMENT"
        return result
    if status == "INACTIVE":
        return {
            "success": False,
            "reason": "USER_INACTIVE"
        }
    if status == "ACTIVE":
        # Verify PIN without creating a normal authentication session
        pin_result = verify_pin_only(employee_id, entered_pin)
        if not pin_result["success"]:
            return pin_result
        # Check whether an admin requested credential re-enrollment
        pending = get_pending_reenrollment(user_id)
        if pending is not None:
            credential_type = pending[0]
            if credential_type == "FACE":
                result = create_face_reenrollment_session(user_id)
            elif credential_type == "RFID":
                result = create_rfid_reenrollment_session(user_id)
            elif credential_type == "FINGERPRINT":
                result = create_fingerprint_reenrollment_session(user_id)
            else:
                return {
                    "success": False,
                    "reason": "INVALID_REENROLLMENT_TYPE"
                }
            if not result["success"]:
                return result
            result["flow"] = "REENROLLMENT"
            result["credential_type"] = credential_type
            return result
        result = authenticate_pin(employee_id, entered_pin)
        result["flow"] = "AUTHENTICATION"
        return result
    return {
        "success": False,
        "reason": "INVALID_USER_STATUS"
    }