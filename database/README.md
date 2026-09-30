# Admin Dashboard

This folder contains the web-based administrator dashboard for the biometric access control system.

The dashboard provides a simple interface for managing users, reviewing access attempts, initiating credential reenrollment, and managing administrator authentication.

It communicates directly with the FastAPI backend using HTTP requests.

## Overview

The dashboard was built using:

- HTML
- CSS
- JavaScript
- FastAPI REST endpoints
- JWT bearer authentication

No frontend framework is required.

The interface is intentionally lightweight so it can be served directly alongside the FastAPI application.

## Features

The dashboard allows an administrator to:

- log in securely
- view registered users
- create new users
- edit user information
- change user status
- request credential reenrollment
- review access attempts
- change the administrator password
- log out

The dashboard does not directly communicate with the ESP32 or biometric sensors.

All actions are sent through the backend API.

## Architecture

The dashboard acts as the administrative frontend of the system.

```text
Administrator
     ↓
Web Dashboard
     ↓
FastAPI Backend
     ↓
PostgreSQL
