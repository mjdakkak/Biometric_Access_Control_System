
# Backend

This folder contains the FastAPI backend for the biometric access control system.

The backend is the central decision-making layer of the project. It coordinates the ESP32 kiosk, PostgreSQL database, administrator dashboard, biometric verification logic, enrollment workflows, authentication sessions, and credential lifecycle.

The backend was designed around a thin-edge / cloud-backend architecture. The ESP32 handles physical sensors and user interaction, while the backend maintains identity state, performs authorization decisions, stores persistent records, and controls the authentication and enrollment state machines.

---

# Responsibilities

The backend is responsible for:

- employee ID and PIN verification
- RFID authentication
- face verification
- fingerprint ownership verification
- two-factor authentication
- randomized second-factor selection
- first-time enrollment
- face reenrollment
- RFID reenrollment
- fingerprint reenrollment
- authentication session management
- enrollment session management
- access-attempt logging
- user-state management
- administrator authentication
- JWT generation and validation
- device authentication
- PostgreSQL communication
- fingerprint slot allocation
- kiosk state routing
- request/response normalization
- cloud deployment support
- coordination with the physical ESP32 kiosk

---

# Architecture

The backend sits between the physical kiosk and the persistent system state.

```text
Physical User
     ↓
ESP32 Kiosk
     ↓
HTTP Requests
     ↓
FastAPI Backend
     ↓
Service Layer
     ↓
Repository / Database Layer
     ↓
PostgreSQL
```

The administrator interface follows a separate path:

```text
Administrator
     ↓
Web Dashboard
     ↓
FastAPI Admin API
     ↓
PostgreSQL
```

The face-recognition path is:

```text
ESP32 Camera
     ↓
Image Upload
     ↓
FastAPI
     ↓
OpenCV Decoding
     ↓
InsightFace
     ↓
Face Embedding
     ↓
Euclidean Distance
     ↓
Verification Decision
```

---

# Design Philosophy

The system intentionally separates physical hardware responsibilities from backend decision-making.

The ESP32 is responsible for:

- reading RFID cards
- interacting with the fingerprint sensor
- capturing camera images
- displaying screens
- receiving keypad input
- controlling hardware
- following backend state transitions

The backend is responsible for:

- deciding whether a credential is valid
- determining which user is involved
- determining the next authentication step
- maintaining user status
- maintaining enrollment state
- storing persistent credential records
- validating fingerprint slot ownership
- performing face verification
- logging authentication outcomes

This keeps the firmware from containing unnecessary business logic.

---

# Backend Modules

The backend is divided into several Python modules.

```text
backend/
├── admin_service.py
├── auth_service.py
├── db.py
├── enrollment_service.py
├── kiosk_service.py
├── main.py
└── repositories.py
```

Each module has a specific responsibility.

---

# `main.py`

`main.py` defines the FastAPI application and acts as the API boundary.

Its responsibilities include:

- application initialization
- route definitions
- request validation
- Pydantic request models
- response normalization
- device authentication
- administrator-route protection
- face-image upload handling
- CORS configuration
- static dashboard serving
- session-cleanup lifecycle tasks

The FastAPI application is created with a lifespan handler that starts a background cleanup task.

Conceptually:

```text
FastAPI starts
     ↓
session cleanup loop starts
     ↓
application serves requests
     ↓
cleanup periodically removes expired sessions
```

---

# `auth_service.py`

`auth_service.py` contains the main authentication logic.

It manages:

- authentication sessions
- session expiration
- RFID first-factor authentication
- PIN first-factor authentication
- face verification
- fingerprint verification
- access-attempt updates
- authentication completion

Authentication sessions are temporary and stored in memory.

A typical authentication session contains information such as:

```text
session_id
user_id
selected second factor
expiration time
associated access attempt
```

---

# `enrollment_service.py`

`enrollment_service.py` manages first-time enrollment and credential reenrollment.

It handles:

- enrollment-session creation
- RFID enrollment
- face enrollment
- fingerprint enrollment
- face reenrollment
- RFID reenrollment
- fingerprint reenrollment
- fingerprint-slot allocation
- enrollment expiration
- enrollment progression

Enrollment sessions are separate from authentication sessions.

---

# `admin_service.py`

`admin_service.py` contains administrator-side operations.

It handles:

- administrator login
- password hashing and verification
- JWT generation
- JWT validation support
- user creation
- user listing
- user editing
- user status changes
- access-attempt retrieval
- reenrollment requests
- administrator password changes

Administrator authentication is separate from kiosk device authentication.

---

# `kiosk_service.py`

`kiosk_service.py` acts as the routing layer for employee ID and PIN entry.

It determines whether a user should enter:

```text
AUTHENTICATION
ENROLLMENT
REENROLLMENT
```

based on:

- whether the user exists
- whether the PIN is valid
- user status
- pending reenrollment state

This allows the kiosk to use a single entry flow while the backend determines what should happen next.

---

# `repositories.py`

`repositories.py` contains database-access operations.

The purpose of this layer is to separate:

```text
business logic
```

from:

```text
SQL / persistence logic
```

This makes the service code easier to read and reduces direct SQL usage throughout the backend.

---

# `db.py`

`db.py` creates PostgreSQL connections.

Connection information is loaded from environment variables.

Conceptually:

```python
host = DB_HOST
port = DB_PORT
database = DB_NAME
user = DB_USER
password = DB_PASSWORD
```

The actual credentials are not stored in the repository.

---

# Authentication Model

The system uses two-factor authentication.

Authentication begins with a first factor and then requires one randomized biometric second factor.

---

# First Factor

The preferred first factor is:

```text
RFID
```

A fallback method is:

```text
employee ID + PIN
```

The system does not use face or fingerprint as the first factor.

This means the backend already has an expected identity before performing biometric verification.

---

# Second Factor

After the first factor succeeds, the backend randomly selects:

```text
FACE
```

or:

```text
FINGERPRINT
```

The selected factor is stored in the authentication session.

The kiosk receives the selected factor through:

```text
next_step
```

and follows the returned backend state.

Conceptually:

```text
First factor succeeds
        ↓
Create authentication session
        ↓
Randomly choose second factor
        ↓
FACE or FINGERPRINT
        ↓
Return next_step
        ↓
ESP32 performs requested biometric step
```

---

# Authentication Session

Authentication sessions are created after successful first-factor authentication.

Each session is identified by:

```text
session_id
```

The session exists only temporarily.

The current authentication timeout is:

```text
60 seconds
```

The timeout is fixed from session creation.

It does not refresh after each follow-up request.

Conceptually:

```text
session created at T = 0
expires at T = 60 seconds
```

A face or fingerprint request received after that deadline is no longer valid.

---

# Why Authentication Sessions Are Temporary

Sessions prevent a successful first factor from remaining valid indefinitely.

Without expiration, an attacker could potentially reuse an earlier successful first-factor state at a much later time.

The short session window ensures that the first and second factors belong to the same immediate authentication attempt.

---

# Session Storage

Authentication sessions are currently stored in backend memory.

This is appropriate for the current single-kiosk prototype.

It means that sessions are lost if:

- the FastAPI process restarts
- Railway redeploys the application
- the server crashes

Persistent distributed sessions would be required for a production multi-instance deployment.

---

# Authentication Flow

A normal authentication flow is:

```text
RFID or ID + PIN
        ↓
Backend validates first factor
        ↓
Create session
        ↓
Choose FACE or FINGERPRINT
        ↓
Return next_step
        ↓
ESP32 performs biometric check
        ↓
Backend validates second factor
        ↓
SUCCESS or FAIL
```

---

# ID + PIN Kiosk Routing

The main ID/PIN kiosk entry point is:

```text
POST /kiosk/id-pin
```

This endpoint is more than a simple PIN check.

It determines the user's current state.

Possible outcomes include:

```text
AUTHENTICATION
ENROLLMENT
REENROLLMENT
```

---

# Active User

For an active user without pending reenrollment:

```text
/kiosk/id-pin
        ↓
PIN verified
        ↓
AUTHENTICATION
        ↓
session_id created
        ↓
FACE or FINGERPRINT selected
```

---

# Pending Enrollment User

For a user with:

```text
PENDING_ENROLLMENT
```

the endpoint starts:

```text
ENROLLMENT
```

instead of authentication.

---

# Pending Reenrollment User

For an active user with a pending credential replacement request:

```text
/kiosk/id-pin
        ↓
PIN verified
        ↓
pending reenrollment detected
        ↓
REENROLLMENT
```

The response also identifies the credential type.

Possible values are:

```text
FACE
RFID
FINGERPRINT
```

---

# User Status

The backend supports:

```text
ACTIVE
INACTIVE
PENDING_ENROLLMENT
```

## `ACTIVE`

The user has completed enrollment and can authenticate normally unless a reenrollment request exists.

## `INACTIVE`

Authentication is blocked.

## `PENDING_ENROLLMENT`

The account exists, but physical credentials have not yet been fully enrolled.

---

# New User Enrollment

New accounts are created through the administrator interface.

A newly created user begins as:

```text
PENDING_ENROLLMENT
```

The user then completes enrollment at the physical kiosk.

The expected sequence is:

```text
ID + PIN
   ↓
RFID
   ↓
FACE
   ↓
FINGERPRINT
   ↓
COMPLETE
```

The ESP32 does not need to hardcode this sequence.

It follows the backend's:

```text
next_step
```

response.

---

# Enrollment Sessions

Enrollment uses:

```text
enrollment_session_id
```

rather than the normal authentication:

```text
session_id
```

The two session types are deliberately separate.

The enrollment timeout is:

```text
300 seconds
```

or:

```text
5 minutes
```

This longer timeout gives the user enough time to complete physical enrollment steps.

---

# RFID Enrollment

During RFID enrollment:

1. the backend requests an RFID step
2. the physical reader reads the full card UID
3. the ESP32 sends the UID to the backend
4. the backend checks uniqueness
5. the UID is associated with the user
6. the enrollment state advances

RFID UIDs are unique in PostgreSQL.

A placeholder database value does not represent a physically enrolled card.

---

# Face Enrollment

Face enrollment uses five image captures.

The backend requests the following head positions:

```text
LOOK_STRAIGHT
TURN_SLIGHTLY_LEFT
TURN_MORE_LEFT
TURN_SLIGHTLY_RIGHT
TURN_MORE_RIGHT
```

Each successful image produces a normalized InsightFace embedding.

Conceptually:

```text
Capture 1 → E1
Capture 2 → E2
Capture 3 → E3
Capture 4 → E4
Capture 5 → E5
```

The embeddings are combined into the enrolled representation.

The resulting face representation is stored in PostgreSQL.

---

# Why Five Face Captures Are Used

A single enrollment image may overrepresent one head orientation.

The multi-capture process introduces controlled pose variation during enrollment.

This improves the stored representation's ability to tolerate normal changes in orientation during authentication.

The ML evaluation is documented in:

```text
../ml/README.md
```

---

# Face Authentication

Face authentication is performed entirely in the backend.

The pipeline is:

```text
ESP32 camera
     ↓
image upload
     ↓
FastAPI
     ↓
OpenCV decoding
     ↓
InsightFace detection
     ↓
normalized 512D embedding
     ↓
load enrolled embedding
     ↓
Euclidean distance
     ↓
threshold comparison
```

The current verification threshold is:

```text
0.95
```

The threshold was selected using offline genuine/impostor evaluation and then applied during hardware integration.

---

# Face Verification

Face verification is one-to-one.

The backend already knows which user should be authenticated.

It compares:

```text
incoming face
```

against:

```text
that user's enrolled face representation
```

The backend does not search every stored user to determine identity.

---

# Liveness

The current backend performs:

```text
face verification
```

not:

```text
liveness detection
```

There is no dedicated anti-spoofing or presentation-attack detection module in the current implementation.

This distinction is intentionally documented.

---

# Fingerprint Architecture

Fingerprint matching differs from face matching.

The backend does not receive or compare fingerprint images.

The AS608 sensor performs the actual fingerprint-template matching.

The backend stores only:

```text
user ↔ template slot
```

mappings.

---

# Fingerprint Enrollment

During fingerprint enrollment:

1. the backend allocates an available sensor slot
2. the ESP32 enrolls the fingerprint into that physical AS608 slot
3. the backend stores the slot mapping
4. the process repeats for the second fingerprint

Each user is normally enrolled with two fingerprint templates.

The backend uses sensor slots:

```text
1–162
```

---

# Fingerprint Authentication

The fingerprint sensor performs the biometric comparison.

The process is:

```text
finger placed on sensor
        ↓
AS608 matches template
        ↓
sensor returns slot
        ↓
ESP32 sends slot to backend
        ↓
backend verifies slot belongs to expected user
```

The backend therefore verifies ownership of the matched slot.

It does not perform fingerprint-feature matching itself.

---

# Database Mapping vs Physical Template

An important distinction is:

```text
database mapping
≠
physical fingerprint template
```

A database row such as:

```text
user_id = 10
template_slot = 24
```

only means the backend expects that template to exist in slot 24.

The AS608 must actually contain the fingerprint template.

The physical sensor and database must therefore remain synchronized.

---

# Credential Reenrollment

The backend supports reenrollment for:

```text
FACE
RFID
FINGERPRINT
```

An administrator creates a pending reenrollment request.

The next time that user enters their ID and PIN, normal authentication is interrupted and the backend starts the appropriate reenrollment flow.

---

# Face Reenrollment

Face reenrollment repeats the multi-capture face enrollment process.

A new face representation is generated and replaces the previous credential after successful completion.

---

# RFID Reenrollment

RFID reenrollment replaces the user's RFID UID.

The new UID must remain globally unique in the credential table.

---

# Fingerprint Reenrollment

Fingerprint reenrollment requires additional care because the actual templates exist on the physical sensor.

The intended safety model is:

```text
old templates remain valid
        ↓
allocate new slots
        ↓
enroll replacement fingerprints
        ↓
confirm replacements exist
        ↓
request deletion of old slots
        ↓
ESP32 deletes physical templates
        ↓
ESP32 confirms deletion
        ↓
backend removes old mappings
```

This prevents the old fingerprints from being removed before valid replacements exist.

---

# Fingerprint Recovery

Fingerprint recovery logic exists as a separate integration concern because physical sensor operations and database operations are not inherently atomic.

For example:

```text
physical template written
but
backend response lost
```

can produce a temporary mismatch between sensor state and backend state.

The hardware-side recovery design documents how committed and uncommitted slot changes should be reconciled.

See:

```text
../hardware/docs/FINGERPRINT_RECOVERY_PROPOSAL.md
```

---

# Request IDs

ESP-facing requests include:

```text
request_id
```

The backend echoes the same value in the response.

This allows the firmware to detect:

- stale responses
- duplicated responses
- conflicting asynchronous responses

Conceptually:

```text
ESP sends request_id = req-123
        ↓
backend processes request
        ↓
backend returns request_id = req-123
        ↓
ESP verifies response belongs to current request
```

---

# Flow State

Responses may contain:

```text
flow
```

Possible values include:

```text
AUTHENTICATION
ENROLLMENT
REENROLLMENT
```

The flow tells the ESP32 which state machine is active.

---

# `next_step`

The backend uses:

```text
next_step
```

to tell the ESP32 what action should happen next.

Possible values include:

```text
RFID
FACE
FINGERPRINT
DELETE_OLD_FINGERPRINTS
COMPLETE
```

This allows the backend to remain authoritative over the workflow.

---

# Response Normalization

ESP-facing responses are normalized so that important context fields remain consistent.

Responses may include:

```text
success
request_id
flow
session_id
enrollment_session_id
credential_type
current_step
next_step
next_prompt
```

When a successful workflow reaches its final state, the backend returns:

```text
next_step = COMPLETE
```

---

# Device Authentication

Kiosk-facing routes require a device API key.

The ESP32 sends:

```text
X-Device-Key: <DEVICE_API_KEY>
```

The backend validates the key before processing protected kiosk endpoints.

The actual value is stored in an environment variable.

---

# Single-Kiosk Assumption

The current prototype assumes:

```text
one physical kiosk
```

The authentication sessions and fingerprint-slot mappings are not explicitly bound to a unique `device_id`.

For the current single-kiosk system, the shared device API key is sufficient for the project scope.

A future multi-kiosk version should introduce explicit device identities and bind sessions to the kiosk that created them.

---

# Administrator Authentication

Administrator access uses a separate authentication mechanism from the ESP32.

Administrators log in through:

```text
POST /admin/login
```

The backend verifies the stored password hash and issues a JWT.

The dashboard then sends:

```text
Authorization: Bearer <token>
```

with protected requests.

---

# JWT Authentication

The JWT contains administrator identity and expiration information.

The signing secret is stored in:

```text
JWT_SECRET
```

The real secret is not committed to the repository.

---

# Administrator Operations

The backend supports:

- creating users
- listing users
- editing user information
- changing user status
- viewing access attempts
- requesting face reenrollment
- requesting RFID reenrollment
- requesting fingerprint reenrollment
- changing the administrator password

The frontend implementation is documented in:

```text
../dashboard/README.md
```

---

# Password Security

PINs and administrator passwords are not stored as plaintext.

The backend stores hashes.

Administrator password verification uses Argon2.

This prevents the original password from being directly recovered from the database.

---

# Access Attempt Logging

Authentication attempts are recorded in PostgreSQL.

The backend records information such as:

```text
user
start time
finish time
first factor
first-factor result
second factor
second-factor result
overall result
failure reason
```

Possible overall results include:

```text
SUCCESS
FAIL
EXPIRED
```

---

# Authentication Approval vs Physical Door State

A backend result of:

```text
SUCCESS
```

means:

```text
authentication was approved
```

It does not prove:

```text
the physical lock actually opened
```

Authentication state and physical actuator state are separate concepts.

A production system could add explicit hardware acknowledgment or door-state sensing if physical opening needs to be independently audited.

---

# PostgreSQL Integration

The backend uses PostgreSQL for persistent state.

The database stores:

```text
users
PIN credentials
RFID credentials
face embeddings
fingerprint slot mappings
access attempts
reenrollment requests
administrator accounts
```

The full database design is documented in:

```text
../database/README.md
```

---

# Database Connection

The backend uses:

```text
psycopg
```

for PostgreSQL access.

Connection values are loaded from:

```text
DB_HOST
DB_PORT
DB_NAME
DB_USER
DB_PASSWORD
```

---

# Environment Variables

The backend depends on several environment variables.

```text
DB_HOST
DB_PORT
DB_NAME
DB_USER
DB_PASSWORD
JWT_SECRET
DEVICE_API_KEY
```

The real `.env` file is excluded from Git.

The repository contains:

```text
.env.example
```

to document the required configuration without exposing credentials.

---

# Cloud Deployment

The backend was deployed on Railway.

The deployment architecture included:

```text
Railway FastAPI service
        ↓
Railway PostgreSQL service
```

The FastAPI service connects to PostgreSQL through Railway-provided environment variables.

---

# Production Start Command

The deployed backend is started using Uvicorn.

Conceptually:

```bash
python -m uvicorn backend.main:app --host 0.0.0.0 --port $PORT
```

Railway supplies the runtime port through:

```text
$PORT
```

---

# OpenCV Deployment

The cloud backend uses headless OpenCV.

This avoids dependencies on desktop graphical libraries that are unnecessary in a server environment.

The backend only needs OpenCV for image processing tasks such as:

- decoding uploaded images
- preparing images for the face-recognition pipeline

It does not require graphical functions such as desktop display windows.

---

# Background Cleanup

FastAPI starts a periodic cleanup task when the application begins.

The cleanup task checks for:

- expired authentication sessions
- expired enrollment sessions

Conceptually:

```text
every 10 seconds
     ↓
check auth sessions
     ↓
check enrollment sessions
     ↓
expire stale state
```

This prevents temporary state from remaining indefinitely in memory.

---

# Expired Authentication

When an authentication session expires, the corresponding access attempt can be marked:

```text
EXPIRED
```

The in-memory session is then removed.

A later request using the removed session ID may therefore appear as an unknown session.

---

# Error Handling

The backend returns structured failures for normal application conditions such as:

- invalid credentials
- inactive users
- unknown sessions
- expired state
- invalid fingerprint ownership
- unsuccessful face verification
- pending reenrollment

Unexpected server failures result in standard FastAPI server errors and are investigated through deployment logs.

---

# API Testing

FastAPI automatically provides Swagger documentation.

During local development:

```text
http://127.0.0.1:8000/docs
```

The Swagger interface was used extensively before hardware integration.

It allowed the backend flows to be tested without requiring the ESP32 for every request.

---

# Hardware Integration Testing

After cloud deployment, the backend was tested with the real physical kiosk.

The integration test covered:

- ESP32 startup
- Wi-Fi connectivity
- Nextion interaction
- kiosk ID/PIN requests
- backend routing
- enrollment state transitions
- RFID flow
- fingerprint sensor integration
- face capture
- face upload
- user activation
- active-user authentication
- access-attempt logging

A camera integration issue occurred during testing and was resolved.

The backend state machine behaved correctly during the integrated hardware test.

---

# Cloud Validation

Before connecting the ESP32, the same flows were tested manually through Swagger.

This allowed validation of:

```text
/kiosk/id-pin
RFID enrollment
face enrollment
fingerprint enrollment
authentication
administrator login
access attempts
password changes
```

This staged approach reduced the number of simultaneous unknowns during hardware integration.

---

# Dashboard Integration

The administrator dashboard is served alongside the backend.

FastAPI mounts the dashboard as static content.

This allows the frontend and backend to share the same deployed domain.

The dashboard uses relative API paths such as:

```javascript
fetch("/admin/login")
```

instead of hardcoded localhost addresses.

---

# Security Boundaries

The project separates two authorization domains.

## Administrator

Uses:

```text
JWT bearer token
```

## ESP32 kiosk

Uses:

```text
X-Device-Key
```

The ESP32 does not use the administrator JWT.

The dashboard does not use the kiosk device key.

---

# Current Security Scope

The current implementation is suitable for a prototype and demonstration system.

Implemented controls include:

- hashed passwords
- hashed PINs
- JWT administrator authorization
- device API-key protection
- session expiration
- credential uniqueness constraints
- user-state checks
- multi-factor authentication
- access-attempt logging
- hidden environment secrets

---

# Current Limitations

The current backend intentionally remains within prototype scope.

Notable limitations include:

- authentication sessions are stored only in memory
- the current architecture assumes one kiosk
- sessions are not explicitly device-bound
- no dedicated rate limiting is implemented
- face liveness detection is not implemented
- the backend records authentication approval rather than physical door-open confirmation
- production-scale distributed session storage is not implemented
- advanced recovery handling remains a separate integration concern
- cloud infrastructure is designed for demonstration rather than high availability

These limitations are documented rather than hidden.

---

# Future Backend Improvements

Possible future improvements include:

- Redis-backed session storage
- per-device kiosk identities
- device-to-session binding
- per-device fingerprint-slot namespaces
- upload-size validation
- stricter image validation
- rate limiting
- structured centralized logging
- automated database migrations
- production secret rotation
- multi-instance deployment support
- physical lock acknowledgments
- door-state sensing
- dedicated liveness detection
- hardware recovery state persistence
- formal API versioning

---

# Local Development

Install project dependencies:

```bash
pip install -r requirements.txt
```

Create a local environment file based on:

```text
.env.example
```

Start FastAPI:

```bash
python -m uvicorn backend.main:app --reload
```

The API is normally available at:

```text
http://127.0.0.1:8000
```

Swagger:

```text
http://127.0.0.1:8000/docs
```

---

# Backend File Summary

| File | Responsibility |
|---|---|
| `main.py` | FastAPI application, routes, middleware, request handling, device protection |
| `auth_service.py` | Authentication sessions and first/second-factor verification |
| `enrollment_service.py` | Enrollment and reenrollment state machines |
| `admin_service.py` | Administrator authentication and management operations |
| `kiosk_service.py` | ID/PIN routing into authentication, enrollment, or reenrollment |
| `repositories.py` | PostgreSQL query and persistence operations |
| `db.py` | Database connection creation |

---

# Backend State Summary

The backend coordinates several major states:

```text
USER STATE
ACTIVE
INACTIVE
PENDING_ENROLLMENT
```

```text
FLOW
AUTHENTICATION
ENROLLMENT
REENROLLMENT
```

```text
SECOND FACTOR
FACE
FINGERPRINT
```

```text
NEXT STEP
RFID
FACE
FINGERPRINT
DELETE_OLD_FINGERPRINTS
COMPLETE
```

This state-based design allows the ESP32 to remain relatively simple while the backend controls workflow progression.

---

# Final Backend Validation

The backend was first tested independently through local and cloud API calls and was later integrated with the real physical components.

The completed integration demonstrated that the backend could successfully coordinate:

```text
physical input
     ↓
ESP32
     ↓
cloud API
     ↓
authentication/enrollment state
     ↓
PostgreSQL
     ↓
response
     ↓
ESP32 user interface
```

The successful hardware test confirmed that the backend state machine and the physical kiosk could operate together as a complete system.

---
