# Backend and Cloud API

The backend is the part of the **Biometric Access Control System** that decides what happens after someone uses the kiosk. The ESP32 reads the sensors and displays the next instruction; the FastAPI application checks credentials, maintains the current authentication or enrollment session, and records the result in PostgreSQL.

The project uses a single ESP32-S3 kiosk, a Nextion display, an RFID reader, an AS608 fingerprint sensor, and an ESP32 camera. FastAPI and PostgreSQL were deployed on Railway and tested with the physical kiosk. The application also serves the [administrator dashboard](../dashboard/README.md).

## Getting started

Run these commands from the **repository root**, not from inside `backend/`.

### 1. Install the Python dependencies

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
```

On Linux or macOS, activate the environment with `source .venv/bin/activate`. The repository's pinned dependencies are the versions used for the project; use a compatible Python environment when reproducing the installation.

### 2. Create the database

Create an empty PostgreSQL database and apply [the schema](../database/schema.sql):

```bash
psql -h localhost -U YOUR_DB_USER -d YOUR_DB_NAME -f database/schema.sql
```

This creates the tables and employee ID sequence, **not** an administrator account or physical sensor credentials. See the [database setup notes](../database/README.md#setup).

### 3. Configure `.env`

Create `.env` in the repository root using [`.env.example`](../.env.example) as a reference:

```dotenv
DB_HOST=localhost
DB_PORT=5432
DB_NAME=your_database
DB_USER=your_database_user
DB_PASSWORD=your_database_password
JWT_SECRET=replace_with_a_long_random_secret
DEVICE_API_KEY=replace_with_a_separate_random_device_key
```

These are placeholders, not working credentials. Do not commit your actual `.env` file, JWT secret, database password, or device key.

### 4. Create the first administrator

Administrator creation is deliberately a **local operation**, not an open registration endpoint. The existing `create_admin()` function hashes the password with Argon2, checks for a duplicate username, and requires a password of at least eight characters.

From the repository root, with the environment configured and the database schema installed, run:

```powershell
python -c "import getpass; from backend.admin_service import create_admin; username=input('Admin username: '); password=getpass.getpass('Admin password: '); print(create_admin(username, password))"
```

Check that the response reports `success: true`. If it does not, check the returned reason and database configuration. Use an administrator password you have not placed in the repository. If setting up an existing Railway database, run this operation in an authorized environment connected to **that database**, not against a different local database.

### 5. Start FastAPI

```powershell
python -m uvicorn backend.main:app --reload
```

- API: `http://127.0.0.1:8000`
- Interactive API reference: `http://127.0.0.1:8000/docs`
- Dashboard login: `http://127.0.0.1:8000/dashboard/login.html`

The Railway start command uses the port supplied by the platform:

```bash
python -m uvicorn backend.main:app --host 0.0.0.0 --port $PORT
```

## Architecture

The ESP32 handles the hardware; the backend owns the workflow and persistent records. Separating these responsibilities lets the firmware respond to a `next_step` rather than independently deciding whether a user is authorized.

```text
                  Administrator
                       |
                  Web dashboard
                       | JWT
                       v
ESP32 kiosk ------> FastAPI backend <----> PostgreSQL
RFID, keypad        routing/services       users, credentials,
AS608, camera       in-memory sessions     access logs, admins
Nextion screen            |
                         +----> OpenCV / InsightFace
                         |      face verification
                         |
                         +----> flow + next_step ----> ESP32
```

PostgreSQL stores the durable user and credential records. Active authentication and enrollment sessions live in the FastAPI process, so they are lost when that process restarts. Fingerprint **templates** live on the AS608; the database only stores their assigned slot numbers.

### Source files

| File | Role |
| --- | --- |
| `main.py` | API routes, request models, face-image processing, middleware, and cleanup lifecycle |
| `kiosk_service.py` | Routes ID/PIN input into authentication, initial enrollment, or reenrollment |
| `auth_service.py` | First-factor checks, second-factor verification, session expiration, and access results |
| `enrollment_service.py` | Enrollment stages, reenrollment stages, and fingerprint-slot allocation |
| `admin_service.py` | Administrator login, tokens, user management, and reenrollment requests |
| `repositories.py` | SQL queries and database operations |
| `db.py` | PostgreSQL connections loaded from environment variables |

## Authentication

The system uses two factors. **RFID** is the normal first factor; **five-digit employee ID and four-digit PIN** are the fallback. After the first factor is accepted, the backend randomly requests either face or fingerprint verification for the **same user**.

```text
RFID or employee ID + PIN
          |
   first factor valid
          |
  auth session created
          |
      FACE or FINGERPRINT
          |
    verify expected user
          |
   SUCCESS / FAIL / EXPIRED
```

### First factor and kiosk routing

`POST /auth/rfid` starts RFID-first authentication using the card's full UID. `POST /kiosk/id-pin` checks the entered ID and PIN and also considers the user's account state:

- `ACTIVE`, no pending replacement: start `AUTHENTICATION` and choose the second factor.
- `PENDING_ENROLLMENT`: start first-time `ENROLLMENT` at the RFID step.
- `ACTIVE`, with a pending credential replacement: start `REENROLLMENT` for the requested credential.
- `INACTIVE`, unknown user, or invalid credentials: reject the request.

The direct `/auth/pin` and `/enroll/start` routes, if retained in the deployed revision, are development entry points. The normal ID/PIN kiosk path is `/kiosk/id-pin`.

### Face verification

The ESP32 uploads a camera image. OpenCV decodes it; InsightFace detects the face and produces a normalized 512-dimensional embedding. The backend compares it with the expected user's enrolled embedding using Euclidean distance and the configured threshold of **0.95**. This is one-to-one verification, not a search for an unknown person among every user. The [ML README](../ml/README.md) explains how the threshold was investigated.

### Fingerprint verification

The AS608 matches a finger against templates stored on the **physical sensor** and reports the matching slot. The ESP32 sends that slot to the backend; the backend checks whether it belongs to the user identified by the first factor. The backend does not compare raw fingerprint data.

### Session lifetime and access logs

Authentication sessions expire **60 seconds after creation**. The deadline is fixed: the face or fingerprint step does not extend it. A cleanup task checks for expired authentication and enrollment sessions approximately every ten seconds. Once an expired session has been removed from memory, another request with the old ID may return `UNKNOWN_SESSION_ID`.

The `access_attempt` table records the factors, individual results, start and finish times, overall result, and failure reason where applicable. `SUCCESS` means the backend **approved authentication**. It is not independent evidence that the solenoid moved or the door physically opened.

## First-time enrollment

An administrator creates a user with status `PENDING_ENROLLMENT`. The user enters their ID and PIN at the kiosk, receiving a separate `enrollment_session_id` and the first `next_step`.

```text
ID + PIN -> RFID -> five face captures -> two fingerprints -> COMPLETE
```

### RFID

The RFID reader returns the card's full UID. The backend checks the database's unique-UID constraint and associates the card with the user. A placeholder UID inserted during API testing is not evidence that a real card was enrolled.

### Face

The backend prompts for five captures in this order:

1. `LOOK_STRAIGHT`
2. `TURN_SLIGHTLY_LEFT`
3. `TURN_MORE_LEFT`
4. `TURN_SLIGHTLY_RIGHT`
5. `TURN_MORE_RIGHT`

InsightFace produces a normalized embedding for each accepted image. The service averages those five vectors and **normalizes the average again** before storing the resulting representation. The intention is to include multiple head orientations; the offline experiments did not independently test whether this outperforms single-image enrollment.

### Fingerprints

The backend assigns two free slots in its configured **1–162** range. The ESP32 must actually create the corresponding templates on the AS608; a successful database insert alone cannot create a physical template. After the required steps complete, the user's status changes to `ACTIVE`.

### Enrollment timeout

In the available enrollment implementation, **initial enrollment and all three reenrollment modes receive a fixed 300-second (five-minute) deadline when the session is created**. Completing an individual RFID, face, or fingerprint step does not refresh this deadline. Authentication has its own, shorter 60-second deadline. If you change either policy in the code, update the firmware expectations and this section together.

## Credential reenrollment

An administrator can request `RFID`, `FACE`, or `FINGERPRINT` reenrollment. At the next ID/PIN entry, the kiosk receives `flow: REENROLLMENT`, the requested `credential_type`, an `enrollment_session_id`, and the next required step.

- **RFID:** read and validate the replacement card UID.
- **Face:** repeat the five-image capture sequence and replace the stored representation on completion.
- **Fingerprint:** allocate two new slots and keep the old templates until the replacement process reaches its deletion step.

Fingerprint replacement is a two-system operation: the backend manages database mappings while the AS608 manages physical templates. The flow requests deletion of the old physical slots and waits for `/enroll/fingerprint/confirm-old-deleted` before retiring the old mappings. **This planned replacement sequence should not be confused with automatic recovery after an unexpected restart or lost response.** The [hardware recovery proposal](../hardware/docs/FINGERPRINT_RECOVERY_PROPOSAL.md) discusses that separate problem. The recovery-application switch was disabled during the reported integration stage; do not assume automatic reconciliation is active without a later verified test.

## API and kiosk contract

All ESP-facing requests send `X-Device-Key`. JSON requests use `Content-Type: application/json`; image uploads use `multipart/form-data`. The ESP creates a new `request_id` for each HTTP request, and the backend echoes it so the firmware can ignore stale responses.

| Endpoint | Purpose |
| --- | --- |
| `POST /kiosk/id-pin` | Route an employee ID and PIN to the appropriate workflow |
| `POST /auth/rfid` | Begin RFID-first authentication |
| `POST /auth/face` | Verify a face for an authentication session |
| `POST /auth/fingerprint` | Check the matched fingerprint slot |
| `POST /enroll/rfid` | Enroll or replace an RFID UID |
| `POST /enroll/face` | Process a face enrollment capture |
| `POST /enroll/fingerprint` | Process an assigned fingerprint slot |
| `POST /enroll/fingerprint/confirm-old-deleted` | Confirm old physical fingerprint slots were deleted |
| `POST /admin/login` | Verify administrator credentials and issue a JWT |

A normal authentication flow uses `session_id`; enrollment and reenrollment use `enrollment_session_id`. The returned `flow` is `AUTHENTICATION`, `ENROLLMENT`, or `REENROLLMENT`. Possible `next_step` values include `RFID`, `FACE`, `FINGERPRINT`, `DELETE_OLD_FINGERPRINTS`, and `COMPLETE`.

### Example: ID/PIN starts enrollment

**Request** to `POST /kiosk/id-pin`:

```http
X-Device-Key: <DEVICE_API_KEY>
Content-Type: application/json
```

```json
{
  "employee_id": "00003",
  "pin": "1234",
  "request_id": "test-001"
}
```

**Response**, based on a successful cloud integration test (the IDs are illustrative):

```json
{
  "success": true,
  "user_id": 3,
  "enrollment_session_id": "e7f7f07b-99e8-43b6-9145-277c85686348",
  "current_step": "RFID",
  "next_step": "RFID",
  "flow": "ENROLLMENT",
  "request_id": "test-001"
}
```

The firmware should retain the returned `enrollment_session_id` and send it in subsequent enrollment requests. It should **not** substitute an authentication `session_id`.

### Example: face upload

`POST /auth/face` uses these exact multipart form fields: `request_id`, `session_id`, and `image`.

```bash
curl -X POST "http://127.0.0.1:8000/auth/face" \
  -H "X-Device-Key: YOUR_DEVICE_API_KEY" \
  -F "request_id=test-002" \
  -F "session_id=SESSION_ID_FROM_FIRST_FACTOR" \
  -F "image=@capture.jpg;type=image/jpeg"
```

For `POST /enroll/face`, use `enrollment_session_id` **instead of** `session_id`, along with `request_id` and `image`.

### Example: invalid face image

The face endpoint returns an application-level failure if OpenCV cannot decode the submitted image:

```json
{
  "success": false,
  "reason": "INVALID_IMAGE",
  "request_id": "test-002",
  "flow": "AUTHENTICATION",
  "session_id": "SESSION_ID_FROM_FIRST_FACTOR"
}
```

This example illustrates the failure shape, not a complete catalog of error codes. An expired session may already have been removed and produce `UNKNOWN_SESSION_ID`; consult the running API's [`/docs`](http://127.0.0.1:8000/docs) and current service code for exact request schemas and other failure reasons.

## Administrator and device security

`POST /admin/login` checks credentials and issues a JWT. **Protected administrator endpoints** require `Authorization: Bearer <JWT>`. The ESP32 uses the separate `X-Device-Key` header and does not use an administrator token.

Administrator passwords and user PINs are stored as hashes. Secrets are read from the environment, not from committed configuration files. This prototype uses one device key for its single kiosk; sessions and fingerprint-slot mappings are **not explicitly tied to a separate device ID**. The backend also does not implement dedicated face liveness or photo-attack detection.

## Deployment and testing

The API and PostgreSQL were deployed as separate Railway services. FastAPI serves the static dashboard from `/dashboard`, so the browser and API can share one origin. Headless OpenCV is used for server-side image processing.

The backend was tested locally and through Swagger before the cloud database was connected. Cloud tests covered administrator login, new-user enrollment, transition to `ACTIVE`, authentication, and access-attempt history. The subsequent hardware integration exercised ESP32 Wi-Fi, Nextion responses, fingerprint interaction, face-image uploads, and the main workflow transitions; an initial camera issue was resolved during testing.

The tests establish that the **main prototype path works**, not that every crash-recovery scenario, anti-spoofing attack, or physical door-opening outcome has been independently validated.

## Known limitations

- Authentication and enrollment sessions are in memory and do not survive process restarts or support independent backend workers without shared storage.
- The current deployment assumes one kiosk and does not bind sessions to individual device identities.
- Face verification does not perform dedicated liveness detection.
- Image-size limits, inference concurrency, rate limiting, and cleanup of all credentials from abandoned enrollment are areas for future hardening.
- Backend approval is logged separately from physical proof that the door opened.
- Automatic recovery from interrupted fingerprint writes or deletions requires separate end-to-end validation.

