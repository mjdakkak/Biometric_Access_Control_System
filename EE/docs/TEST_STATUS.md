# Verification status

## Observed in the user's earlier bench runs

- ESP32 target build succeeded after the v8.1 certificate-header fix.
- Firmware v8.2 booted and reported Wi-Fi/clock readiness.
- Physical Nextion TFT upload and idle UI worked; ID/PIN messages reached the controller/backend.
- HTTPS /kiosk/id-pin returned authentication routes.
- /auth/fingerprint returned HTTP 200 with success=false; slot/reason diagnosis remained incomplete.
- Camera PID 0x3660, fresh VGA JPEG and decoding worked. First-stage detector returned zero candidates; the raw preview subsequently showed an upside-down image.
- Camera orientation was changed to vflip=1, hmirror=0. The uploaded code contains the reviewed checked initialization; a new upright frame and detection success have not been confirmed here.
- HW-131 delivered approximately 5.014 V at the running display; combined power/thermal behavior remains unverified.

## Checks performed for this UI change

- 43,726 host assertions across representative errors and 10,000 randomized inputs: formatting, page/line width, text limits, sanitization, escaped newlines, and overflow pages.
- The same formatter test passed AddressSanitizer/UndefinedBehaviorSanitizer.
- Updated screen.cpp passed C++11 syntax checking using the existing simulated Arduino/FreeRTOS headers.
- Source diff: only screen.cpp modified, FailureDisplay.h added; other uploaded firmware source and the HMI preserved byte-for-byte.

These are desktop tests, not a Nextion emulator or real hardware certification. No actual PlatformIO build, TFT compile, or device upload of this UI change was performed here. No authenticated API request or lock actuation was performed.

## Still to verify

- Build/upload the changed source using the actual ESP32 toolchain.
- Generate/upload the current HMI TFT; confirm font 5, forced line breaks, alignment, full reasons, page indicators and six-second reading time on the display.
- Verify upright preview and local detection, then real enrollment/authentication using the actual camera and consenting participant.
- Verify AS608 communication/address range and backend/template inventory, including uncertain-operation recovery.
- Resolve rapid/excessive heating before extended tests; confirm supply voltages under full load.
- Complete authenticated recovery transport/maintenance orchestration after backend agreement.
- Complete protected relay/lock wiring and behavior before enabling output.

Not in MVP: dashboard remote unlock and low-power wake-up. Backend success is not physical door opening.
