# Backend

This folder contains the FastAPI backend for the biometric access control system.

The backend is responsible for authentication, enrollment and reenrollment flows, credential validation, access logging, administrator operations, and communication between the ESP32 kiosk and PostgreSQL database.

## Overview

The backend acts as the main decision-making layer of the system.

The ESP32 handles the physical interface and sensors, while the backend determines:

- which authentication flow should start
- whether credentials are valid
- which second factor should be requested
- whether a user should enter authentication, enrollment, or reenrollment
- how fingerprint template slots are assigned
- when authentication or enrollment sessions expire
- how access attempts are recorded
- how administrators manage users and credentials

The backend was developed using FastAPI and PostgreSQL and deployed to Railway for cloud testing.

## Main Responsibilities

The backend handles:

- employee ID and PIN authentication
- RFID authentication
- face authentication
- fingerprint authentication
- new-user enrollment
- face reenrollment
- RFID reenrollment
- fingerprint reenrollment
- authentication session management
- enrollment session management
- access-attempt logging
- administrator authentication
- user management
- device authentication
- cloud database communication
- ESP32 state-machine coordination

## System Architecture

The system follows a thin-edge / cloud-backend architecture.

The ESP32 kiosk is responsible for:

- user interaction
- Nextion display control
- RFID reading
- fingerprint sensor communication
- camera capture
- physical hardware control
- sending requests to the backend

The backend is responsible for:

- authentication decisions
- credential validation
- database access
- face embedding comparison
- enrollment state
- reenrollment state
- user status
- access logs
- administrator operations

A typical flow is:

```text
ESP32
  ↓
FastAPI backend
  ↓
PostgreSQL
  ↓
FastAPI response
  ↓
ESP32 state machine
