# Backend interface — single-kiosk MVP

The eight public function signatures in src/BackendInterface.h are preserved. BackendStubs.cpp retains its old filename for build compatibility but contains the real HTTPS client.

| Hook | POST endpoint | Payload |
|---|---|---|
| sendIdPinToBackend | /kiosk/id-pin | JSON employee_id, pin, request_id |
| sendRfidAuthToBackend | /auth/rfid | JSON rfid_uid, request_id |
| sendFaceAuthToBackend | /auth/face | Multipart request_id, session_id, image JPEG |
| sendFingerprintAuthToBackend | /auth/fingerprint | JSON session_id, matched_template_slot, request_id |
| sendEnrollmentRfid | /enroll/rfid | JSON enrollment_session_id, rfid_uid, request_id |
| sendEnrollmentFace | /enroll/face | Multipart request_id, enrollment_session_id, image JPEG |
| sendEnrollmentFingerprint | /enroll/fingerprint | JSON enrollment_session_id, template_slot, request_id |
| confirmOldFingerprintsDeleted | /enroll/fingerprint/confirm-old-deleted | JSON enrollment_session_id, request_id |

All requests use X-Device-Key from local secrets.h. HTTPS verification remains enabled. The deployment address is in NetworkConfig.h and is not a secret.

Follow the backend next_step. Authentication uses session_id; enrollment/reenrollment uses enrollment_session_id. Fixed lifetimes are 60 s and 300 s respectively. Until server expiry metadata exists, the client anchors a conservative whole-workflow deadline to the initial request queue time. Per-request deadlines remain shorter.

Backend slot policy is 1–162. Physical AS608 addressing/capacity validation is separate; never change the upper address test blindly. A completed no-match search is JSON null, not slot zero. The sensor must actually contain a template; a seeded database mapping alone is insufficient.

Face hooks synchronously borrow a detected, padded JPEG through getBackendFaceImage(). They must not free or retain that pointer after returning. Identity embeddings and their comparison stay on the backend. Detection is not recognition, pose verification, or liveness proof.

Reenrollment stores the two new fingerprints before deleting the specified old templates. A request ID is correlation, not proof of durable server-side idempotency. Recovery requires the separate agreed maintenance protocol; it is not implemented merely by the normal confirm-old-deleted endpoint.

The current backend intentionally assumes one physical kiosk: it has device-key protection but does not maintain device-scoped sessions/template mappings. Treat adding or swapping a sensor/kiosk as a change requiring reassessment. SUCCESS means authentication approved, not physical door-open confirmation.
