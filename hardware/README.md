# EE — ESP32-S3 biometric kiosk

Electrical/embedded work for the joint **Biometric Access Control System**. The partner's backend/dashboard remain in `mjdakkak/Biometric_Access_Control_System`; this EE directory is intentionally self-contained and can be copied into that repository without moving its other files.

## Open the right folder

Open **`EE/firmware/`** in VS Code/PlatformIO. Its `platformio.ini` and `src/` must remain together. Copy `src/secrets.h.example` to `src/secrets.h` locally, enter your private Wi-Fi/device key, then build/upload. Do not commit the populated file or firmware binaries.

## Contents

| Directory | Purpose |
|---|---|
| `firmware/` | Current uploaded C++ baseline plus readable denial messages |
| `nextion/` | Editable screen project in the downloadable handoff; model/font/event notes |
| `hardware/` | Pin allocation and qualified prototype wiring notes |
| `docs/` | Setup, API mapping, test status, recovery proposal, and changelog |

## Scope and status

The single-kiosk MVP has completed functional testing on the assembled prototype. Testing covered RFID reading, Nextion interaction, camera-based face enrollment and authentication, fingerprint enrollment and authentication, HTTPS communication, and relay-controlled lock actuation.

The backend controls authentication and enrollment workflows. Dashboard remote unlocking and low-power wake-up are outside the current MVP scope. Fingerprint recovery integration remains deferred and disabled.

See `docs/TEST_STATUS.md` for the tested configuration and results. Backend authentication approval and physical door-opening confirmation remain distinct.
