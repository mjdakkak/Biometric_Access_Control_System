# Setup

1. Open `EE/firmware` in VS Code, not the parent repository folder.
2. Keep the supplied `platformio.ini` (Espressif32 6.12.0 / Arduino-ESP32 2.0.17 target). It was not changed in this organization update.
3. Copy `src/secrets.h.example` to `src/secrets.h` locally. Enter a reachable 2.4 GHz SSID, its password, and the backend-issued device key. The key is not an employee PIN.
4. Check `src/HardwareConfig.h` against the actual wiring. This handoff preserves the user's hardware flags and pin allocation; it does not enable the lock.
5. Build, Upload, then Monitor at 115200. A build is not a hardware or API test.
6. In Nextion Editor, use the latest `nextionScreen.HMI`, target NX3224F028_011, compile and export `.tft`. Load it using the previously working FAT32 microSD procedure. Editing C++ does not flash the Nextion.

## Denied-page update

The uploaded HMI already assigns font 5 (Arial16) to page7.t1. That object is 222x74 pixels with txt_maxl=100. It has automatic wrapping disabled; the new firmware explicitly inserts Nextion `\r` line breaks instead.

Only `src/screen.cpp` and the new `src/FailureDisplay.h` are required to apply this UI update to the existing AccessControl project. No library or source-filter changes are needed. Keep the HMI's font IDs and object names unchanged. Re-export/upload the latest HMI if the physical screen still has the older font set.

The firmware sets t1.font=5, left/top alignment, zero character/line spacing, wraps by the supplied font metrics, and shows each error page for six seconds. Rare long messages continue on another numbered page. Known API codes receive readable messages. Mapping occurs only at rendering: the original reason, request IDs, session checks, and backend decisions are untouched.

## Local verification

From the firmware directory on a desktop with a C++ compiler:

```sh
c++ -std=c++11 -Wall -Wextra -Werror -I src test/test_failure_display.cpp -o /tmp/test_failure_display
/tmp/test_failure_display
```

This test covers formatting/escaping/bounds, not real Nextion rendering, camera inference, electrical safety, or API behavior.

## Privacy and diagnostics

`camera_snapshot_preview.h` defaults to a temporary, tokenized local HTTP preview. It is not encrypted and is intended only for a trusted private LAN. Do not publish preview URLs or captured faces. Set its existing KIOSK_CAMERA_PREVIEW macro to 0 after diagnosis and rebuild. The source deliberately prevents combining preview and enabled lock output.

Before sharing a ZIP, exclude `secrets.h`, `.pio`, `.vscode`, unredacted logs, face captures, and generated firmware. `.gitignore` does not strip files from an ordinary ZIP.
