# Biometric Access Control System

A single-kiosk access-control prototype combining an ESP32-S3, RFID, a touchscreen, face recognition, and fingerprint verification. The ESP32 manages the hardware and user interface, while a cloud backend manages users, verifies credentials, and records access attempts.

## How it works

A user starts by scanning an RFID card or entering an employee ID and PIN on the Nextion display. The backend checks the request and selects face or fingerprint verification. After successful authentication, the ESP32 activates the relay for a timed lock-release pulse and displays the result.

Face processing is split between the device and the server. The ESP32 captures a JPEG, checks for one detected face, and crops it. The backend uses InsightFace to generate and compare face embeddings. Fingerprints are matched locally by the AS608; the backend checks the returned template slot against the current user and session.

For first-time enrollment, the sequence is **RFID → five face captures → two fingerprints**. Credential-specific reenrollment lets an existing user replace a card, face profile, or fingerprints. The backend determines the next step throughout the workflow.

The [backend interface](hardware/docs/BACKEND_INTERFACE.md) describes the requests and responses.

## Hardware used

| Component | Role |
|---|---|
| **Freenove ESP32-S3-WROOM board, V1.2, N16R8** | Main controller; runs the firmware and connects to the backend over Wi-Fi. |
| **OV3660 camera** | Captures face images through the board's camera connector. |
| **Nextion NX3224F028_011** | 2.8-inch touchscreen for ID/PIN entry, capture prompts, and results. |
| **RC522 RFID reader** | Reads card UIDs for authentication and enrollment. |
| **AS608 fingerprint sensor** | Captures, stores, and matches fingerprint templates. |
| **Two-channel 5 V relay module** | Uses one channel to switch the lock's separate power circuit. |
| **12 V solenoid lock** | Demonstrates the physical lock-release action. |
| **S8050 NPN transistor** | Interfaces the ESP32's control output with the relay input. |
| **1 kΩ and 10 kΩ resistors** | Set the transistor drive and relay-input bias. |
| **1N4007 diode** | Provides flyback suppression across the solenoid coil. |
| **HW-131 breadboard power module** | Provides the prototype's 3.3 V and 5 V peripheral rails. |
| **USB power and an adjustable 12 V adapter** | Supply the controller, peripherals, and separate lock circuit. |
| **Breadboards, jumper wires, and connectors** | Support assembly and prototype wiring. |

A microSD card and USB card reader are used to load the compiled Nextion screen project. They are setup tools, not required for normal kiosk operation.

In the prototype arrangement, computer USB powers the Freenove and camera, the HW-131 supplies the peripherals, and the 12 V adapter supplies the lock through the relay contacts. See [pinout and power notes](hardware/wiring/PINOUT_AND_POWER.md) for connections and electrical precautions. The 12 V lock wiring stays off the solderless breadboards.

## Software

| Area | Tools and implementation |
|---|---|
| **Embedded firmware** | C++, Arduino-ESP32, FreeRTOS, and PlatformIO in VS Code |
| **Display project** | Nextion Editor |
| **Backend API** | Python and FastAPI |
| **Face recognition** | InsightFace, OpenCV, and NumPy |
| **Database** | PostgreSQL |
| **Admin dashboard** | HTML, CSS, and JavaScript |
| **Communication** | HTTPS, JSON requests, and multipart JPEG uploads |

The firmware separates display handling, network requests, and sensor work into tasks. Request IDs, session deadlines, and cancellation handling prevent an old response from being applied to a later attempt.

The dashboard provides user management and access-attempt records. Authentication and enrollment follow the backend's workflow rather than a fixed sequence of local screen changes.

## Repository layout

| Folder | Contents |
|---|---|
| [`backend/`](backend/) | API routes, authentication, enrollment, and user-management services |
| [`dashboard/`](dashboard/) | Browser-based administration interface |
| [`database/`](database/) | Database schema and entity-relationship diagram |
| [`hardware/`](hardware/) | Firmware, Nextion project, wiring notes, and hardware documentation |
| [`ml/`](ml/) | Face-recognition experiments and related project material |
| [`images/`](images/) | Project photos and screenshots added for this README |

Open **`hardware/firmware/`** in VS Code to work on the PlatformIO project. Keep `platformio.ini` and `src/` together. Create a private `src/secrets.h` from `src/secrets.h.example`; never commit the populated file.

The editable display project is [`hardware/nextion/nextionScreen.HMI`](hardware/nextion/nextionScreen.HMI). Compile and upload it separately using Nextion Editor.

## Project roles

**Electrical and embedded:** firmware, sensor integration, display communication, power distribution, and relay/lock control.

**Backend and application software:** API, database, administration dashboard, and embedding-based face verification.

## Status and scope

**The core single-kiosk prototype has passed functional testing**, as confirmed by the project team. Testing covered RFID reading, touchscreen interaction, face and fingerprint enrollment/authentication, HTTPS communication, readable error messages, and relay-controlled lock actuation. See [test status](hardware/docs/TEST_STATUS.md).

Fingerprint recovery integration remains deferred and disabled. Dashboard remote unlocking and low-power wake-up are outside the current MVP. Backend authentication approval is not a measurement that the physical door opened.

This is a functional prototype, not a certified building-access installation. Keep credentials, fingerprint templates, private camera captures, and unredacted logs out of the public repository.

## Project photos

### Hardware and wiring

*Photo to be added.*

<!-- After uploading the photo, replace the line above with:
![Prototype hardware and wiring](images/wiring.jpg)
-->

### Nextion interface

*Photo to be added.*

<!-- After uploading the screenshot, replace the line above with:
![Nextion touchscreen interface](images/nextion.jpg)
-->

### Administration dashboard

*Screenshot to be added.*

<!-- After uploading the screenshot, replace the line above with:
![Administration dashboard](images/dashboard.png)
-->
