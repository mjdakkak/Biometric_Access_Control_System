# Backend and Cloud API

The backend is the decision-making component of the **Biometric Access Control System**. It coordinates the ESP32 kiosk, biometric verification, enrollment and reenrollment, administrator operations, and PostgreSQL persistence. The kiosk operates the physical devices; FastAPI validates credentials and tells the kiosk which step to perform next.

This implementation is designed for a **single-kiosk prototype** and was tested with a Railway-hosted API and PostgreSQL database.

## Contents

- [Architecture](#architecture)
- [Getting started](#getting-started)
- [Project files](#project-files)
- [Authentication](#authentication)
- [Enrollment](#enrollment)
- [Reenrollment](#reenrollment)
- [API and kiosk contract](#api-and-kiosk-contract)
- [Security](#security)
- [Deployment and testing](#deployment-and-testing)
- [Limitations](#limitations)

## Architecture

```text
                    Administrator
                         |
                    Web dashboard
                         | JWT
                         v
ESP32 kiosk ------> FastAPI backend <------> PostgreSQL
 RFID / PIN        services and state        users / credentials
 AS608             |                       sessions' access logs
 camera            +--> OpenCV / InsightFace
 Nextion           |        face verification
                  +--> next_step response --> ESP32
```

The backend performs **one-to-one face verification**: the first factor identifies the expected user, and the submitted face is compared with that user's stored embedding. The AS608 performs fingerprint matching locally; the backend checks that the returned sensor slot belongs to the expected user.

The [dashboard](../dashboard/README.md) is served as static files by FastAPI. Persistent data is stored in [PostgreSQL](../database/README.md); authentication and enrollment sessions are held temporarily in server memory.

## Getting started

Run these commands from the repository root.

### 1. Create a Python environment

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

On Linux or macOS, activate the environment with `source .venv/bin/activate`.

### 2. Set up PostgreSQL

Create a fresh PostgreSQL database and apply the [SQL schema](../database/schema.sql). With the `psql` client, for example:

```bash
psql -h localhost -U YOUR_DB_USER -d YOUR_DB_NAME -f database/schema.sql
```

The schema initializes an empty database; it does not import test users or create fingerprint templates on the AS608. See [Database setup](../database/README.md#setup).

### 3. Configure the environment

Create a local `.env` file using [`.env.example`](../.env.example) as a template. Set the following variables:

| Variable | Purpose |
| --- | --- |
| `DB_HOST` | PostgreSQL host |
| `DB_PORT` | PostgreSQL port |
| `DB_NAME` | Database name |
| `DB_USER` | Database user |
| `DB_PASSWORD` | Database password |
| `JWT_SECRET` | Signing secret for administrator tokens |
| `DEVICE_API_KEY` | Key required by ESP-facing endpoints |

Do not commit `.env`, live API keys, passwords, or production credentials. An administrator account must be provisioned separately before dashboard login; the SQL schema does not create a default administrator.

### 4. Start the API

```bash
python -m uvicorn backend.main:app --reload
```

- API: `http://127.0.0.1:8000`
- Interactive API documentation: `http://127.0.0.1:8000/docs`
- Dashboard login: `http://127.0.0.1:8000/dashboard/login.html`

The production-style start command used for Railway is:

```bash
python -m uvicorn backend.main:app --host 0.0.0.0 --port $PORT
```

## Project files

| File | Responsibility |
| --- | --- |
| `main.py` | FastAPI routes, request models, response formatting, image uploads, middleware, and application lifecycle |
| `auth_service.py` | First/second-factor verification, authentication sessions, and access-attempt completion |
| `enrollment_service.py` | First-time enrollment, credential replacement, session progression, and fingerprint-slot allocation |
| `kiosk_service.py` | ID/PIN routing into authentication, enrollment, or reenrollment |
| `admin_service.py` | Administrator login, JWTs, user management, and reenrollment requests |
| `repositories.py` | SQL queries and persistence operations |
| `db.py` | PostgreSQL connections using environment variables |

## Authentication

### First factor

RFID is the normal starting method; employee ID plus a four-digit PIN is the fallback. The RFID reader sends the **full card UID**. The PIN path begins at `POST /kiosk/id-pin`, which also checks whether the user needs enrollment or reenrollment.

### Randomized second factor

For an `ACTIVE` user, the backend creates a session and randomly chooses `FACE` or `FINGERPRINT`. The response contains `session_id` and `next_step` so the firmware knows what to request. The second factor must match the **same expected user** identified by the first factor.

- **Face:** the ESP32 uploads an image; InsightFace creates an embedding; the backend compares it with that user's enrolled embedding using Euclidean distance. The current decision threshold is `0.95`. See [Face Recognition Evaluation](../ml/README.md).
- **Fingerprint:** the AS608 matches a stored template and returns its slot number. The backend verifies the user's assignment to that slot; it does not receive or compare fingerprint templates.

### Session duration and logging

Authentication sessions expire **60 seconds after creation**. This is a fixed deadline; later biometric requests do not refresh it. A cleanup task runs approximately every 10 seconds and removes expired in-memory sessions. Once removed, an old ID may return `UNKNOWN_SESSION_ID`. A server restart also clears active sessions.

Access attempts record the factors used, results, times, and overall `SUCCESS`, `FAIL`, or `EXPIRED` status. `SUCCESS` means the **backend approved authentication**, not that a sensor confirmed the physical door opened.

## Enrollment

An administrator creates a user with status `PENDING_ENROLLMENT`. On the kiosk, a successful ID/PIN entry starts `ENROLLMENT` and returns an `enrollment_session_id`.

```text
ID + PIN -> RFID -> five FACE captures -> two FINGERPRINTS -> COMPLETE
```

### RFID

The physical reader supplies the card's full UID. The backend records it for the user and enforces the database's UID-uniqueness rule. Placeholder UIDs used in API tests are not physical credentials.

### Face

The backend requests these five head positions:

1. `LOOK_STRAIGHT`
2. `TURN_SLIGHTLY_LEFT`
3. `TURN_MORE_LEFT`
4. `TURN_SLIGHTLY_RIGHT`
5. `TURN_MORE_RIGHT`

InsightFace produces a normalized 512-dimensional embedding for each accepted capture. In the available enrollment implementation, the five vectors are averaged and the result is **normalized again** before storage. This multi-view procedure is intended to include pose variation; the offline graphs do not independently measure its benefit over single-image enrollment.

### Fingerprint

The backend allocates two available AS608 slots in its configured range of **1–162**. The ESP32 must physically create the templates on the sensor. The database holds user-to-slot mappings, **not the templates themselves**. A seeded database mapping cannot be used for authentication unless the corresponding sensor template actually exists.

After all required steps finish, the backend marks the user `ACTIVE`.

### Enrollment timeout

Enrollment uses a separate, longer session from authentication. Earlier implementation snapshots initialize enrollment sessions at **300 seconds (five minutes)**, but some reenrollment versions use different deadlines. Confirm the final deployed `enrollment_service.py` for the exact reenrollment deadline and whether successful steps refresh the expiry before relying on a uniform timeout in firmware.

## Reenrollment

An administrator can request replacement of `FACE`, `RFID`, or `FINGERPRINT`. For an active user with a pending request, `/kiosk/id-pin` routes to `REENROLLMENT` rather than normal authentication and supplies `credential_type` and `next_step`.

- **Face:** take five replacement captures and update the stored representation upon successful completion.
- **RFID:** read a replacement card, validate UID uniqueness, and update the credential.
- **Fingerprint:** keep the old sensor templates while enrolling two replacements in new slots. The workflow then requests old-template deletion and an ESP32 confirmation before retiring the old database mappings.

**Fingerprint replacement is not the same as crash recovery.** Losing connectivity after a physical sensor write or delete can leave the sensor and database out of sync. The [hardware recovery proposal](../hardware/docs/FINGERPRINT_RECOVERY_PROPOSAL.md) documents this concern. Automatic recovery/receipt application should not be described as completed unless the final firmware and backend implementations have both been verified. Confirm physical slots before any destructive deletion test.

## API and kiosk contract

ESP-facing requests require `X-Device-Key`. JSON requests also use `Content-Type: application/json`; face uploads are multipart requests.

| Endpoint | Purpose |
| --- | --- |
| `POST /kiosk/id-pin` | Check ID/PIN and select the correct workflow |
| `POST /auth/rfid` | Begin RFID-first authentication |
| `POST /auth/face` | Verify a face for an existing auth session |
| `POST /auth/fingerprint` | Verify the matched sensor slot |
| `POST /enroll/rfid` | Enroll or replace an RFID credential |
| `POST /enroll/face` | Process an enrollment face capture |
| `POST /enroll/fingerprint` | Enroll replacement or first-time fingerprint slots |
| `POST /enroll/fingerprint/confirm-old-deleted` | Confirm old physical templates were deleted |
| `POST /admin/login` | Obtain an administrator JWT |

The ESP supplies a fresh `request_id` for each request; the backend echoes it. Firmware can use this to discard stale responses. **Normal authentication** uses `session_id`; **enrollment and reenrollment** use `enrollment_session_id`.

Typical flow values are `AUTHENTICATION`, `ENROLLMENT`, and `REENROLLMENT`; `next_step` can request `RFID`, `FACE`, `FINGERPRINT`, `DELETE_OLD_FINGERPRINTS`, or `COMPLETE`. Consult [`/docs`](http://127.0.0.1:8000/docs) on a running local instance for current request fields and schemas. The development-only `/auth/pin` and `/enroll/start` entry points, if retained, are not the normal kiosk entry path.

## Security

Administrator endpoints require a JWT bearer token, while device-facing endpoints require a separate `X-Device-Key`. PINs and administrator passwords are stored as hashes. The `.env` file is excluded from Git; `.env.example` contains configuration names only.

The current static device key authenticates the **kiosk client**, but individual auth sessions and fingerprint mappings are **not explicitly bound to a device identity**. The design assumes one kiosk. The backend also does not implement dedicated face liveness detection.

## Deployment and testing

The API and PostgreSQL were deployed as separate Railway services. The static dashboard is served by FastAPI on the same domain, and headless OpenCV is used for server-side image processing.

The project was first exercised locally and through Swagger, then tested against the cloud database. Physical integration later exercised ESP32 Wi-Fi, Nextion responses, fingerprint interaction, camera uploads, and the main authentication/enrollment state transitions. An ESP camera integration issue encountered during testing was resolved.

These tests confirm the main **prototype integration path**; they are not evidence of automatic fingerprint crash recovery, production availability, protection against photo-based attacks, or independently sensed door opening.

## Limitations

- Sessions are in memory and are lost on process restart; this design is not suitable for multiple independent backend workers without shared session storage.
- The project assumes one kiosk and does not use per-device session binding.
- Dedicated face liveness detection is not implemented.
- Upload limits, malformed-image handling, inference concurrency, rate limiting, and abandoned-enrollment cleanup are areas for further hardening.
- Authentication approval is logged separately from any physical proof of door actuation.
- Recovery after interrupted fingerprint writes/deletions requires separate implementation and integration validation.

## Related documentation

- [Admin Dashboard](../dashboard/README.md)
- [Database and ERD](../database/README.md)
- [Face Recognition Evaluation](../ml/README.md)
- [Hardware and Firmware](../hardware/README.md)
