# Biometric Access Control System


A single-kiosk access-control prototype combining an ESP32-S3, RFID, a touchscreen, face recognition, and fingerprint verification. The ESP32 manages the hardware and user interface, while a cloud-hosted backend manages users, verifies credentials, and records access attempts.

## How It Works

A user starts by scanning an RFID card or entering an employee ID and PIN on the Nextion display. The backend checks the request and randomly selects face or fingerprint verification as the second factor. After successful two-factor authentication, the ESP32 activates a relay for a timed lock-release pulse.

The system uses a distributed approach for biometric processing:
-   **Face Recognition**: The ESP32 captures a JPEG, performs initial validation (detecting a single face), and crops it. The backend then uses a pre-trained InsightFace model to generate a face embedding and compares it to the user's stored embedding.
-   **Fingerprint Matching**: The AS608 sensor matches a user's fingerprint locally against templates stored on the device. It returns the matched template slot number to the backend, which verifies that the slot belongs to the authenticated user.

First-time enrollment follows the sequence: **RFID → Five Face Captures → Two Fingerprints**. The system also supports re-enrollment for existing users to replace a specific credential (card, face, or fingerprints).

```text
                    Administrator
                         |
                    Web dashboard
                         | JWT
                         v
ESP32 kiosk ------> FastAPI backend <------> PostgreSQL
 RFID / PIN        services and state        users / credentials
 AS608             |                       sessions / access logs
 camera            +--> OpenCV / InsightFace
 Nextion           |        face verification
                  +--> next_step response --> ESP32
```

## Hardware Used

| Component                        | Role                                                                |
| -------------------------------- | ------------------------------------------------------------------- |
| **Freenove ESP32-S3-WROOM**      | Main controller; runs firmware and connects to backend via Wi-Fi.     |
| **OV3660 Camera**                | Captures face images via the board's camera connector.              |
| **Nextion NX3224F028_011**       | 2.8-inch touchscreen for UI, ID/PIN entry, and prompts.             |
| **RC522 RFID Reader**            | Reads card UIDs for authentication and enrollment.                  |
| **AS608 Fingerprint Sensor**     | Captures, stores, and matches fingerprint templates.                |
| **5V Relay Module**              | Switches the lock's separate 12V power circuit.                     |
| **12V Solenoid Lock**            | Demonstrates the physical lock-release action.                      |
| **S8050 NPN Transistor**         | Interfaces the ESP32's output with the relay input.                 |
| **Resistors (1kΩ, 10kΩ)**        | Set transistor drive and relay-input bias.                          |
| **1N4007 Diode**                 | Provides flyback suppression for the solenoid coil.                 |
| **HW-131 Power Module**          | Provides 3.3V and 5V rails for peripherals.                         |
| **Adapters & Wires**             | USB for the controller, 12V for the lock, and breadboard wiring.    |

Refer to the [pinout and power notes](hardware/wiring/PINOUT_AND_POWER.md) for detailed connection and electrical safety information.

## Software

| Area                    | Tools and Implementation                                |
| ----------------------- | ------------------------------------------------------- |
| **Embedded Firmware**   | C++, Arduino-ESP32, FreeRTOS, PlatformIO in VS Code       |
| **Display Project**     | Nextion Editor                                          |
| **Backend API**         | Python, FastAPI                                         |
| **Face Recognition**    | InsightFace, OpenCV, NumPy                              |
| **Database**            | PostgreSQL                                              |
| **Admin Dashboard**     | HTML, CSS, and JavaScript                               |
| **Communication**       | HTTPS, JSON, Multipart JPEG uploads, and Serial (Nextion) |

## Repository Structure

| Directory | Contents |
| --- | --- |
| [`backend/`](./backend/) | FastAPI API routes, authentication, enrollment, and user-management services. |
| [`dashboard/`](./dashboard/) | Browser-based administration interface for managing users and viewing access logs. |
| [`database/`](./database/) | PostgreSQL schema (`schema.sql`) and an entity-relationship diagram. |
| [`hardware/`](./hardware/) | ESP32 firmware, Nextion HMI project, wiring notes, and hardware documentation. |
| [`ml/`](./ml/) | Jupyter notebooks and documentation related to face-recognition evaluation. |

## Setup and Installation

### Prerequisites
- Git
- Python 3.8+ and `venv`
- PostgreSQL server
- [PlatformIO IDE for VS Code](https://platformio.org/install/ide?install=vscode)
- [Nextion Editor](https://nextion.tech/nextion-editor/)

### 1. Database Setup
1.  Create a new PostgreSQL database.
2.  Apply the schema using the `psql` client or another tool:
    ```bash
    psql -h YOUR_HOST -U YOUR_USER -d YOUR_DB_NAME -f database/schema.sql
    ```

### 2. Backend Setup
1.  Navigate to the repository root and create a Python virtual environment:
    ```bash
    python -m venv .venv
    source .venv/bin/activate  # On Windows, use: .\.venv\Scripts\Activate.ps1
    ```
2.  Install the required dependencies:
    ```bash
    pip install -r requirements.txt
    ```
3.  Create a `.env` file by copying `.env.example` and fill in your database credentials and secrets:
    ```bash
    cp .env.example .env
    # Edit .env with your configuration
    ```
4.  The system requires an admin user for the dashboard. A creation script is not provided, but you can add one to the `admin_user` table manually or by extending `admin_service.py`.
5.  Start the backend server:
    ```bash
    uvicorn backend.main:app --reload
    ```
    The API will be available at `http://127.0.0.1:8000`.

### 3. Hardware and Firmware
1.  **Firmware:**
    - Open the `hardware/firmware/` directory in VS Code with the PlatformIO extension.
    - Copy `hardware/firmware/src/secrets.h.example` to `hardware/firmware/src/secrets.h`.
    - Edit `secrets.h` to add your Wi-Fi credentials and the `DEVICE_API_KEY` you set in the `.env` file.
    - Use PlatformIO to build and upload the firmware to your ESP32-S3 board.

2.  **Nextion Display:**
    - Open `hardware/nextion/nextionScreen.HMI` in the Nextion Editor.
    - Compile the project to generate a `.tft` file.
    - Copy the `.tft` file to a FAT32-formatted microSD card and use it to flash the display.

### 4. Accessing the System
- Once the backend is running, the administrator dashboard is available at `http://127.0.0.1:8000/dashboard/login.html`.
- The ESP32 kiosk will connect to the Wi-Fi and backend API automatically on startup.

## Project Roles
- **Electrical and Embedded:** Firmware development, sensor integration, display communication, power distribution, and relay/lock control.
- **Backend and Application Software:** API design, database schema, administration dashboard, and server-side face verification logic.

## Status and Scope
This project is a **functional single-kiosk prototype**. Core features, including RFID/PIN entry, face/fingerprint enrollment and authentication, and lock actuation, have passed functional testing.

**Out of Scope for MVP:**
- Automated recovery for interrupted fingerprint operations.
- Remote unlocking from the administrator dashboard.
- Low-power or wake-on-event modes.

This system is a proof-of-concept and is not certified for use as a security-critical building access installation.

## Project Photos

### Hardware and Wiring

*Photo to be added.*

<!-- After uploading the photo, replace the line above with:
![Prototype hardware and wiring](images/wiring.jpg)
-->

### Nextion Interface

*Photo to be added.*

<!-- After uploading the screenshot, replace the line above with:
![Nextion touchscreen interface](images/nextion.jpg)
-->

### Administration Dashboard

*Screenshot to be added.*

<!-- After uploading the screenshot, replace the line above with:
![Administration dashboard](images/dashboard.png)
-->
