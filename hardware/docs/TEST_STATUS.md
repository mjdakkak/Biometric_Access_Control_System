# Test status

## Prototype results

**Core MVP functional tests: passed, confirmed by the project owner.**

| Function | Result |
|---|---|
| ESP32 build, upload and startup | Passed |
| Wi-Fi and HTTPS communication | Passed |
| Nextion ID/PIN entry and Denied-page messages | Passed |
| RFID reading | Passed |
| Camera orientation, local face detection and cropping | Passed |
| Face enrollment and authentication | Passed |
| Fingerprint enrollment and authentication | Passed |
| Relay-controlled lock actuation | Passed |

The owner also confirmed that the earlier overheating concern was resolved. These are reported prototype results, not independently repeated safety or environmental qualification. No measured temperature limits, test counts or exact bench-tested firmware revision were supplied.

## Scope

Fingerprint recovery transport and maintenance integration remain deferred. Recovery apply stays disabled. Dashboard remote unlocking and low-power wake-up are outside the MVP. A backend approval is not a door-position measurement.

## This comment/documentation update

- Non-comment C++ tokens match the repository baseline in all 22 source/test files checked (39,984 tokens).
- The message formatter passed 43,726 host checks before and after cleanup, with identical output.
- The cleaned formatter test passed AddressSanitizer and UndefinedBehaviorSanitizer.
- GPIOs, settings, endpoints, messages and control flow were not changed.

No ESP32-toolchain build or hardware rerun was performed for this comment-only update. The committed lock/preview settings were not changed to infer a bench configuration from the reported results.
