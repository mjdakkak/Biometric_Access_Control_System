# Nextion display project

Target: Discovery **NX3224F028_011**, 240x320 portrait. The user's source `nextionScreen.HMI` is included unchanged in the downloadable hardware handoff. The compiled `.TFT` is not included; create it with Nextion Editor and load it using the working FAT32 microSD procedure.

## Denied page verified from the uploaded HMI

- Page: page7.
- Message component: t1; x=9, y=56, w=222, h=74.
- Font resource 5: Arial16 (16-pixel height).
- txt_maxl=100, character/line spacing=0.
- isbr=0 (automatic wrapping off); new firmware inserts explicit line breaks.
- tm0 is disabled; the ESP32 owns result-display timing.

Do not rename page7/t1 or reorder font resources without updating FailureDisplay.h and screen.cpp. The new firmware uses t1.font=5, left/top alignment and three body lines plus a page indicator where needed.

The full HMI was not edited by the error-formatting patch. Compile/export/upload the user's latest HMI if the physical display still contains older fonts. The source structure and runtime rendering were inspected, but Nextion Editor was not run in this environment.

## Repository transfer status

The binary HMI is in the downloadable handoff; it has not yet been uploaded through the current GitHub connector. Add `nextionScreen.HMI` here using GitHub's **Add file -> Upload files**, or copy the complete hardware folder from the handoff into a local repository and commit it. Check its SHA-256 against `nextionScreen.HMI.sha256`.

No screen source should contain real device keys, Wi-Fi passwords or test-user credentials. Do not upload snapshots or private preview URLs.
