# Security and scope notes

This is an unfinished bench prototype, not a production or emergency-egress access-control installation.

- Do not publish local secrets.h, firmware binaries that embed keys, identifying test-account fixtures, face images, fingerprints, or unredacted logs.
- .gitignore only excludes untracked files from normal Git operations. It does not remove secrets already committed and does not sanitize ZIP archives. Rotate any exposed credentials.
- The camera preview is a temporary unencrypted local HTTP diagnostic; its random URL is a bearer link, not public authentication. Disable it after diagnosis.
- The local face detector is not a liveness/identity guarantee. Only the backend verifies embeddings; recognition thresholds are not altered by the UI patch.
- A matched local fingerprint slot must be verified against the active backend session. Pending mutation records are deliberately retained after uncertain operations.
- Request ID echoing prevents mismatched responses but is not durable server-side idempotency.
- Current single-kiosk backend does not explicitly device-scope sessions or slot mappings; do not silently deploy a second kiosk.
- Relay output command, actuator acknowledgement, bolt position and actual door opening are distinct observations. Current SUCCESS means authentication approved.
- Recovery application, lock enablement and any maintenance commands require explicit verification/authorization; a local boolean is not authentication.

Source licensing: retain the joint repository license and honor dependency/model licenses. No new license is asserted for third-party embedded libraries, model data or user resources by this folder organization.
