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

Single kiosk; backend-owned authentication/enrollment sequence. No remote-unlock channel or low-power wake-up in the agreed MVP. Authentication approval is not proof that the physical door opened.

The user reported a successful ESP32 build, Wi-Fi/clock readiness, Nextion communication, and real HTTPS routing. The latest camera orientation correction and this error-display update still require confirmation on the device. Fingerprint recovery HTTP/maintenance orchestration, thermal investigation, protected lock verification, and full enrollment remain open.

The source preserves the uploaded hardware settings: RFID/fingerprint/camera enabled, lock disabled, recovery apply disabled. Disable a peripheral that is not physically ready. Camera preview is a temporary private-LAN diagnostic; disable it after diagnosis. These settings are not a production safety certification.

Read `docs/SETUP.md`, `docs/TEST_STATUS.md`, and `hardware/PINOUT_AND_POWER.md` before powering peripherals.
