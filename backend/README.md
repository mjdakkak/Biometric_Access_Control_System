# Backend

This folder contains the FastAPI backend for the biometric access control system.

The backend is the central decision-making layer of the project. It coordinates the ESP32 kiosk, PostgreSQL database, administrator dashboard, biometric verification logic, enrollment workflows, authentication sessions, and credential lifecycle.

The backend was designed around a thin-edge / cloud-backend architecture. The ESP32 handles physical sensors and user interaction, while the backend maintains identity state, performs authorization decisions, stores persistent records, and controls the authentication and enrollment state machines.

---

# Responsibilities

The backend is responsible for:

- employee ID and PIN verification
- RFID authentication
- face verification
- fingerprint ownership verification
- two-factor authentication
- randomized second-factor selection
- first-time enrollment
- face reenrollment
- RFID reenrollment
- fingerprint reenrollment
- authentication session management
- enrollment session management
- access-attempt logging
- user-state management
- administrator authentication
- JWT generation and validation
- device authentication
- PostgreSQL communication
- fingerprint slot allocation
- kiosk state routing
- request/response normalization
- cloud deployment support
- coordination with the physical ESP32 kiosk

---

# Architecture

The backend sits between the physical kiosk and the persistent system state.

```text
Physical User
     ↓
ESP32 Kiosk
     ↓
HTTP Requests
     ↓
FastAPI Backend
     ↓
Service Layer
     ↓
Repository / Database Layer
     ↓
PostgreSQL
