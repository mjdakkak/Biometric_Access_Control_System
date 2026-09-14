from backend.db import get_connection   

def find_user_by_rfid(rfid_uid):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT u.id, u.employee_id, u.first_name, u.last_name, u.status
        FROM users u
        JOIN rfid_credential r
            ON u.id = r.user_id
        WHERE r.rfid_uid = %s
          AND r.status = 'ACTIVE';
        """,
        (rfid_uid,)
    )
    user = cursor.fetchone()
    cursor.close()
    conn.close()
    return user

def find_user_by_employee_id(employee_id):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
    """
    SELECT u.id, u.employee_id, u.first_name, u.last_name, u.status
    FROM users u
    WHERE u.employee_id = %s
    """,
    (employee_id,))
    user = cursor.fetchone()
    cursor.close()
    conn.close()
    return user

def get_face_embeddings(user_id):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT f.embedding
        FROM face_credential f
        WHERE f.user_id = %s
        """,
        (user_id,)
    )
    result = cursor.fetchone()
    cursor.close()
    conn.close()
    return result

def get_pin_hash(user_id):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT p.pin_hash
        FROM pin_credential p
        WHERE p.user_id = %s
        """,
        (user_id,)
    )
    result = cursor.fetchone()
    cursor.close()
    conn.close()
    return result

def get_fingerprint_template(user_id):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT f.template_slot
        FROM fingerprint_credential f
        WHERE f.user_id = %s
        AND f.status = 'ACTIVE'
        """,
        (user_id,)
    )
    result = cursor.fetchall()
    cursor.close()
    conn.close()
    return result

def find_user_by_id(id):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT * 
        FROM users
        WHERE id = %s
        """,
        (id,)
    )
    user = cursor.fetchone()
    cursor.close()
    conn.close()
    return user 

def create_access_attempt(user_id, first_factor, first_factor_result):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        INSERT INTO access_attempt(
        user_id,
        first_factor,
        first_factor_result
        )
        VALUES (%s, %s, %s)
        RETURNING attempt_id
        """,
        (user_id, first_factor, first_factor_result)
    )

    attempt_id = cursor.fetchone()[0]
    conn.commit()
    cursor.close()
    conn.close()
    return attempt_id

def finish_access_attempt(attempt_id,second_factor,second_factor_result,overall_result,failure_reason):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        UPDATE access_attempt
        SET
            finished_at = CURRENT_TIMESTAMP,
            second_factor = %s,
            second_factor_result = %s,
            overall_result = %s,
            failure_reason = %s
        WHERE attempt_id = %s
        """,
        (second_factor, second_factor_result, overall_result, failure_reason, attempt_id) 
        )
    conn.commit()
    cursor.close()
    conn.close()

def add_rfid_credential(user_id, rfid_uid):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        INSERT INTO rfid_credential (
            user_id,
            rfid_uid,
            status
        )
        VALUES (%s, %s, 'ACTIVE')
        """,
        (
            user_id,
            rfid_uid
        )
    )
    conn.commit()
    cursor.close()
    conn.close()

def add_face_credential(user_id, embedding):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        INSERT INTO face_credential (
            user_id,
            embedding,
            status
        )
        VALUES (%s, %s, 'ACTIVE')
        """,
        (
            user_id,
            embedding
        )
    )
    conn.commit()
    cursor.close()
    conn.close()

def add_fingerprint_credential(user_id, template_slot):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        INSERT INTO fingerprint_credential (
            user_id,
            template_slot,
            status
        )
        VALUES (%s, %s, 'ACTIVE')
        """,
        (user_id,  template_slot)
    )
    conn.commit()
    cursor.close()
    conn.close()

def fingerprint_slot_exists(template_slot):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT 1
        FROM fingerprint_credential
        WHERE template_slot = %s
        AND status = 'ACTIVE'
        """,
        (template_slot,)
    )
    result = cursor.fetchone()
    cursor.close()
    conn.close()
    return result is not None

def set_user_status(user_id, status):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        UPDATE users
        SET status = %s
        WHERE id = %s
        """,
        (status, user_id)
    )
    conn.commit()
    cursor.close()
    conn.close()

def get_all_users():
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT
            id,
            employee_id,
            first_name,
            last_name,
            status
        FROM users
        ORDER BY id
        """
    )
    result = cursor.fetchall()
    cursor.close()
    conn.close()
    return result

def update_user_details( user_id, first_name,last_name):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        UPDATE users
        SET first_name = %s,
            last_name = %s
        WHERE id = %s
        RETURNING id
        """,
        (first_name, last_name,  user_id)
    )
    result = cursor.fetchone()
    conn.commit()
    cursor.close()
    conn.close()
    return result is not None

def get_access_attempts():
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT
            a.attempt_id,
            u.employee_id,
            u.first_name,
            u.last_name,
            a.started_at,
            a.finished_at,
            a.first_factor,
            a.second_factor,
            a.overall_result,
            a.failure_reason
        FROM access_attempt a
        LEFT JOIN users u
            ON a.user_id = u.id
        ORDER BY a.started_at DESC
        """
    )
    results = cursor.fetchall()
    cursor.close()
    conn.close()
    return results

def reset_user_credentials(user_id):
    conn = get_connection()
    cursor = conn.cursor()
    try:
        cursor.execute(   # Saving the fingerprint slots before deleting them from the database so the ESP knows which templates need to be removed
            """
            SELECT template_slot
            FROM fingerprint_credential
            WHERE user_id = %s
            """,
            (user_id,)
        )
        fingerprint_slots = [row[0] for row in cursor.fetchall()]
        cursor.execute(
            """
            DELETE FROM rfid_credential
            WHERE user_id = %s
            """,
            (user_id,)
        )
        cursor.execute(
            """
            DELETE FROM face_credential
            WHERE user_id = %s
            """,
            (user_id,)
        )
        cursor.execute(
            """
            DELETE FROM fingerprint_credential
            WHERE user_id = %s
            """,
            (user_id,)
        )
        cursor.execute(
            """
            UPDATE users
            SET status = 'PENDING_ENROLLMENT'
            WHERE id = %s
            """,
            (user_id,)
        )
        conn.commit()
        return fingerprint_slots
    except Exception:
        conn.rollback()
        raise
    finally:
        cursor.close()
        conn.close()

def replace_face_credential(user_id, embedding_json):
    conn = get_connection()
    cursor = conn.cursor()
    try:
        cursor.execute(
            """
            DELETE FROM face_credential
            WHERE user_id = %s
            """,
            (user_id,)
        )
        cursor.execute(
            """
            INSERT INTO face_credential(
                user_id,
                embedding,
                status
            )
            VALUES (%s, %s, 'ACTIVE')
            """,
            (user_id, embedding_json)
        )
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        cursor.close()
        conn.close()

def replace_rfid_credential(user_id, rfid_uid):
    conn = get_connection()
    cursor = conn.cursor()
    try:
        cursor.execute(
            """
            UPDATE rfid_credential
            SET rfid_uid = %s,
                status = 'ACTIVE'
            WHERE user_id = %s
            """,
            (rfid_uid, user_id)
        )
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        cursor.close()
        conn.close()


def delete_fingerprint_credentials(user_id):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        DELETE FROM fingerprint_credential
        WHERE user_id = %s
        """,
        (user_id,)
    )
    conn.commit()
    cursor.close()
    conn.close()

def set_pending_reenrollment(user_id, credential_type):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        INSERT INTO pending_reenrollment(
            user_id,
            credential_type
        )
        VALUES (%s, %s)

        ON CONFLICT (user_id)
        DO UPDATE SET
            credential_type = EXCLUDED.credential_type,
            created_at = CURRENT_TIMESTAMP
        """,
        (user_id, credential_type))

    conn.commit()
    cursor.close()
    conn.close()


def get_pending_reenrollment(user_id):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT credential_type
        FROM pending_reenrollment
        WHERE user_id = %s
        """,
        (user_id,)
    )
    result = cursor.fetchone()
    cursor.close()
    conn.close()
    return result

def clear_pending_reenrollment(user_id):
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute(
        """
        DELETE FROM pending_reenrollment
        WHERE user_id = %s
        """,
        (user_id,)
    )
    conn.commit()
    cursor.close()
    conn.close()

def get_next_free_fingerprint_slot():
    conn = get_connection()
    cursor = conn.cursor()
    try:
        cursor.execute(
            """
            SELECT template_slot
            FROM fingerprint_credential
            WHERE status = 'ACTIVE'
            ORDER BY template_slot
            """
        )
        used_slots = {row[0] for row in cursor.fetchall()}
        for slot in range(1, 163):  # 162 template slots in fingerprint scanner
            if slot not in used_slots:
                return slot
        return None
    finally:
        cursor.close()
        conn.close()

def delete_specific_fingerprint_credentials(user_id, template_slots):
    if not template_slots:
        return
    conn = get_connection()
    cursor = conn.cursor()
    try:
        cursor.execute(
            """
            DELETE FROM fingerprint_credential
            WHERE user_id = %s
            AND template_slot = ANY(%s)
            """,
            (user_id, template_slots)
        )
        conn.commit()
    finally:
        cursor.close()
        conn.close()