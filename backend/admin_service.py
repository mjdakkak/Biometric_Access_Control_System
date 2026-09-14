from argon2 import PasswordHasher
from argon2.exceptions import VerifyMismatchError
from dotenv import load_dotenv
from datetime import datetime, timedelta, timezone
import os
import jwt
from backend.db import get_connection
from backend.repositories import (
    get_all_users,
    set_user_status,
    update_user_details,
    get_access_attempts,
    reset_user_credentials,
    find_user_by_id
)
load_dotenv()
ph = PasswordHasher()
# JWT settings for administrator dashboard authentication
JWT_SECRET = os.getenv("JWT_SECRET")
JWT_ALGORITHM = "HS256"
JWT_EXPIRATION_MINUTES = 60
# New users remain pending until biometric enrollment is completed
def create_pending_user(pin, first_name, last_name):
    if not pin.isdigit() or len(pin) != 4:
        return {
            "success": False,
            "reason": "INVALID_PIN_FORMAT"
        }
    hash_pin = ph.hash(pin)
    conn = get_connection()
    cursor = conn.cursor()
    try:
        cursor.execute(
            """
            SELECT nextval('employee_id_seq')
            """
        )
        employee_number = cursor.fetchone()[0]
        employee_id = str(employee_number).zfill(5)
        cursor.execute(
            """
            INSERT INTO users (
                employee_id,
                first_name,
                last_name,
                status
            )
            VALUES (
                %s,
                %s,
                %s,
                'PENDING_ENROLLMENT'
            )
            RETURNING id
            """,
            (employee_id, first_name, last_name)
        )
        user_id = cursor.fetchone()[0]
        cursor.execute(
            """
            INSERT INTO pin_credential (
                user_id,
                pin_hash
            )
            VALUES (%s, %s)
            """,
            (user_id, hash_pin)
        )
        conn.commit()
        return {
            "success": True,
            "user_id": user_id,
            "employee_id": employee_id,
            "first_name": first_name,
            "last_name": last_name,
            "status": "PENDING_ENROLLMENT"
        }
    except Exception:
        conn.rollback()
        raise
    finally:
        cursor.close()
        conn.close()

def list_users():
    users = get_all_users()
    user_list = []
    for user in users:
        user_list.append({
            "user_id": user[0],
            "employee_id": user[1],
            "first_name": user[2],
            "last_name": user[3],
            "status": user[4]
        })

    return {
        "success": True,
        "users": user_list
    }

def change_user_status(user_id, new_status):
    if new_status not in ["ACTIVE", "INACTIVE", "PENDING_ENROLLMENT"]:
        return {
            "success": False,
            "reason": "INVALID_STATUS"
        }
    user = find_user_by_id(user_id)
    if user is None:
        return {
            "success": False,
            "reason": "UNKNOWN_USER"
        }
    set_user_status(user_id, new_status)
    return {
        "success": True,
        "user_id": user_id,
        "status": new_status
    }

def update_user(user_id, first_name, last_name):
    updated = update_user_details(user_id, first_name, last_name)
    if not updated:
        return {
            "success": False,
            "reason": "UNKNOWN_USER"
        }
    return {
        "success": True,
        "user_id": user_id,
        "first_name": first_name,
        "last_name": last_name
    }

def list_access_attempts():
    attempts = get_access_attempts()
    attempt_list = []
    for attempt in attempts:
        attempt_list.append({
            "attempt_id": attempt[0],
            "employee_id": attempt[1],
            "first_name": attempt[2],
            "last_name": attempt[3],
            "started_at": attempt[4],
            "finished_at": attempt[5],
            "first_factor": attempt[6],
            "second_factor": attempt[7],
            "overall_result": attempt[8],
            "failure_reason": attempt[9]
        })
    return {
        "success": True,
        "attempts": attempt_list
    }

def reset_credentials(user_id):
    user = find_user_by_id(user_id)
    if user is None:
        return {
            "success": False,
            "reason": "UNKNOWN_USER"
        }
    fingerprint_slots = reset_user_credentials(user_id)
    return {
        "success": True,
        "user_id": user_id,
        "status": "PENDING_ENROLLMENT",
        "fingerprint_slots_to_delete": fingerprint_slots
    }

def find_admin_by_username(username):
    conn = get_connection()
    cursor = conn.cursor()
    try:
        cursor.execute(
            """
            SELECT
                admin_id,
                username,
                password_hash
            FROM admin_user
            WHERE username = %s
            """,
            (username,)
        )

        return cursor.fetchone()
    finally:
        cursor.close()
        conn.close()


# Used locally to create an admin
def create_admin(username, password):
    username = username.strip()
    if not username:
        return {
            "success": False,
            "reason": "INVALID_USERNAME"
        }
    if len(password) < 8:
        return {
            "success": False,
            "reason": "PASSWORD_TOO_SHORT"
        }
    existing_admin = find_admin_by_username(username)
    if existing_admin is not None:
        return {
            "success": False,
            "reason": "ADMIN_ALREADY_EXISTS"
        }
    password_hash = ph.hash(password)
    conn = get_connection()
    cursor = conn.cursor()
    try:
        cursor.execute(
            """
            INSERT INTO admin_user (
                username,
                password_hash
            )
            VALUES (%s, %s)
            RETURNING admin_id
            """,
            (username, password_hash)
        )
        admin_id = cursor.fetchone()[0]
        conn.commit()
        return {
            "success": True,
            "admin_id": admin_id,
            "username": username
        }
    except Exception:
        conn.rollback()
        raise
    finally:
        cursor.close()
        conn.close()

def authenticate_admin(username, password):
    admin = find_admin_by_username(username.strip())
    if admin is None:
        return {
            "success": False,
            "reason": "INVALID_LOGIN"
        }
    admin_id = admin[0]
    db_username = admin[1]
    password_hash = admin[2]
    try:
        ph.verify(password_hash, password)
    except VerifyMismatchError:
        return {
            "success": False,
            "reason": "INVALID_LOGIN"
        }
    token = create_admin_token(admin_id, db_username)
    return {
        "success": True,
        "admin_id": admin_id,
        "username": db_username,
        "token": token,
        "token_type": "bearer",
        "expires_in_minutes": JWT_EXPIRATION_MINUTES
    }

def create_admin_token(admin_id, username):
    if not JWT_SECRET:
        raise RuntimeError("JWT_SECRET is missing from .env")
    now = datetime.now(timezone.utc)
    payload = {
        "admin_id": admin_id,
        "username": username,
        "iat": now,
        "exp": now + timedelta(minutes=JWT_EXPIRATION_MINUTES)
    }
    return jwt.encode(payload, JWT_SECRET, algorithm=JWT_ALGORITHM)

def verify_admin_token(token):
    if not JWT_SECRET:
        return {
            "success": False,
            "reason": "JWT_SECRET_NOT_CONFIGURED"
        }
    try:
        payload = jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALGORITHM])
        return {
            "success": True,
            "admin_id": payload["admin_id"],
            "username": payload["username"]
        }
    except jwt.ExpiredSignatureError:
        return {
            "success": False,
            "reason": "TOKEN_EXPIRED"
        }
    except jwt.InvalidTokenError:
        return {
            "success": False,
            "reason": "INVALID_TOKEN"
        }

def change_admin_password(admin_id, current_password, new_password):
    conn = get_connection()
    cursor = conn.cursor()
    try:
        cursor.execute(
            """
            SELECT password_hash
            FROM admin_user
            WHERE admin_id = %s
            """,
            (admin_id,)
        )
        row = cursor.fetchone()
        if row is None:
            return {
                "success": False,
                "reason": "ADMIN_NOT_FOUND"
            }
        stored_hash = row[0]
        try:
            ph.verify(stored_hash, current_password)
        except VerifyMismatchError:
            return {
                "success": False,
                "reason": "CURRENT_PASSWORD_INCORRECT"
            }
        if len(new_password) < 8:
            return {
                "success": False,
                "reason": "NEW_PASSWORD_TOO_SHORT"
            }
        new_hash = ph.hash(new_password)
        cursor.execute(
            """
            UPDATE admin_user
            SET password_hash = %s
            WHERE admin_id = %s
            """,
            (new_hash, admin_id)
        )
        conn.commit()
        return {
            "success": True
        }
    finally:
        cursor.close()
        conn.close()