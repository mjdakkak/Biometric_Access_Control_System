# Changes in this organization/update

Based on the user-provided `codes & files.zip`, not reconstructed from older patches.

## Firmware changes

- Added `FailureDisplay.h`: bounded, pixel-aware wrapping for the supplied Arial16 font; explicit line breaks; numbered overflow pages; readable mappings for common backend codes; quote/backslash/control-byte sanitization.
- Updated `screen.cpp`: render the denial reason with font 5, left/top alignment and six seconds per message page. The generic 64-character setText helper remains unchanged for other screens.
- Backend API names, request/session deadlines, credential decisions, GPIO assignments, hardware-enable flags, sensor modules, camera orientation, and lock code were not changed.
- The uploaded camera file is byte-identical to the previously reviewed orientation replacement: it checks vflip(1)/hmirror(0) results and logs the resulting settings. Upright-image and detector success still need physical confirmation.

## Organization

- Kept the full PlatformIO project together under EE/firmware.
- Added project-local secrets/build-output ignore rules and an empty secrets template.
- Kept the user HMI byte-for-byte in the downloadable handoff; see nextion/README.md for GitHub transfer status.
- Added setup, wiring, scope, backend, test-status, recovery and privacy notes. No old generated wiring illustration was republished.

No device key, Wi-Fi password, actual test-account table, face photo, or populated secrets.h was included in the supplied source ZIP or this handoff.
