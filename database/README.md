# Database

This folder contains the PostgreSQL schema and database design for the biometric access control system.

The database stores users, credentials, administrator accounts, reenrollment requests, and authentication attempt history.

It does not store raw fingerprint templates. Fingerprint templates remain on the physical AS608 sensor, while PostgreSQL stores the mapping between each user and the sensor slot assigned to that template.

## Files

### `schema.sql`

Contains the SQL used to create the PostgreSQL database structure.

### `biometric_access_erd.drawio`

Contains the entity-relationship diagram for the database.

The ERD shows the relationships between users, biometric credentials, access attempts, administrator accounts, and reenrollment requests.

## Overview

The database is designed around the `users` table.

Each user may have:

- one PIN credential
- one RFID credential
- one face credential
- multiple fingerprint-slot mappings
- multiple access attempts
- an optional pending reenrollment request

Administrator accounts are stored separately.

## Main Tables

The schema includes:

```text
users
face_credential
fingerprint_credential
rfid_credential
pin_credential
access_attempt
pending_reenrollment
admin_user
