CREATE SEQUENCE employee_id_seq
START WITH 1
INCREMENT BY 1;

CREATE TABLE users (
    id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    employee_id VARCHAR(5) UNIQUE NOT NULL,
    first_name VARCHAR(20) NOT NULL,
    last_name VARCHAR(20) NOT NULL,
    status VARCHAR(20) NOT NULL
        CHECK (status IN ('ACTIVE', 'INACTIVE', 'PENDING_ENROLLMENT')),
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE face_credential (
    face_id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id INTEGER NOT NULL,
    embedding TEXT NOT NULL,
    status VARCHAR(20) NOT NULL
        CHECK (status IN ('ACTIVE', 'INACTIVE')),
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE fingerprint_credential (
    fingerprint_id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id INTEGER NOT NULL,
    template_slot INTEGER UNIQUE NOT NULL,
    status VARCHAR(20) NOT NULL
        CHECK (status IN ('ACTIVE', 'INACTIVE')),
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE rfid_credential (
    rfid_id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id INTEGER NOT NULL,
    rfid_uid VARCHAR(50) UNIQUE NOT NULL,
    status VARCHAR(20) NOT NULL
        CHECK (status IN ('ACTIVE', 'INACTIVE')),
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE pin_credential (
    user_id INTEGER PRIMARY KEY,
    pin_hash TEXT NOT NULL,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE access_attempt (
    attempt_id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id INTEGER,
    started_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    finished_at TIMESTAMP,
    first_factor VARCHAR(20) NOT NULL
        CHECK (first_factor IN ('PIN', 'RFID')),
    second_factor VARCHAR(20)
        CHECK (second_factor IN ('FINGERPRINT', 'FACE')),
    first_factor_result VARCHAR(20) NOT NULL
        CHECK (first_factor_result IN ('SUCCESS', 'FAIL')),
    second_factor_result VARCHAR(20)
        CHECK (second_factor_result IN ('SUCCESS', 'FAIL')),
    overall_result VARCHAR(20)
        CHECK (overall_result IN ('SUCCESS', 'FAIL', 'EXPIRED')),
    failure_reason VARCHAR(50),
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE pending_reenrollment (
    user_id INTEGER PRIMARY KEY,
    credential_type VARCHAR(20) NOT NULL
        CHECK (credential_type IN ('FACE', 'RFID', 'FINGERPRINT')),
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id)
);

CREATE TABLE admin_user (
    admin_id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    username VARCHAR(50) UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);