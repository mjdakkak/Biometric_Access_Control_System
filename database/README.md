# PostgreSQL Database

PostgreSQL stores the Biometric Access Control System's user records, credential metadata, administrator accounts, pending reenrollment requests, and authentication-attempt history. The database is the persistent source for the backend, but it does **not** contain fingerprint templates: those are stored on the physical AS608 sensor.

## Files

- [Database schema](schema.sql) — the SQL definition of the project's tables, sequence, relationships, and constraints.
- [Entity-relationship diagram](biometric_access_erd.drawio) — editable database diagram.

If you export the ERD as `biometric_access_erd.png` and place it in this folder, you can display it here with `![Database ERD](biometric_access_erd.png)`.

## Setup

Create a **fresh PostgreSQL database**, then run the schema from the repository root. For example, with the PostgreSQL `psql` client:

```bash
psql -h localhost -U YOUR_DB_USER -d YOUR_DB_NAME -f database/schema.sql
```

Use the correct host, account, and database for your environment. This command initializes schema objects; it does **not** migrate an existing database, create an administrator account, copy test users, or write physical templates to the fingerprint sensor. Local PostgreSQL and Railway-hosted PostgreSQL are separate databases unless data is explicitly transferred.

Configure the backend with `DB_HOST`, `DB_PORT`, `DB_NAME`, `DB_USER`, and `DB_PASSWORD`. The variable names are listed in [`.env.example`](../.env.example), and startup instructions are in the [backend README](../backend/README.md#getting-started).

## Tables

| Table | Stored information |
| --- | --- |
| `users` | Internal user ID, five-digit employee ID, name, status, and creation time |
| `pin_credential` | User-to-hashed-PIN association |
| `rfid_credential` | User-to-card UID association and credential status |
| `face_credential` | User-to-face embedding association and credential status |
| `fingerprint_credential` | User-to-AS608 template-slot mapping and credential status |
| `access_attempt` | First and second factors, results, timestamps, overall outcome, and failure reason |
| `pending_reenrollment` | Pending `FACE`, `RFID`, or `FINGERPRINT` replacement request |
| `admin_user` | Administrator username and password hash |

The schema also defines `employee_id_seq`, which is used when generating employee IDs.

## User identity and relationships

`users.id` is the internal primary key referenced by credential tables. `users.employee_id` is the unique identifier shown at the kiosk and formatted as five digits by the application.

Users can be `PENDING_ENROLLMENT`, `ACTIVE`, or `INACTIVE`. A user has a single PIN row and can have credential records and multiple historical access attempts. Fingerprints are **one-to-many** because normal enrollment assigns two sensor slots per user; the database may also temporarily contain replacement mappings during reenrollment.

The schema enforces uniqueness of RFID UIDs and fingerprint slot numbers. The application manages which credential records are currently active and how replacement is performed. The intended number of active credentials per user is a **workflow rule** and should not be confused with a database constraint unless the SQL explicitly enforces it.

## Where biometrics are stored

### Face

The database stores a numeric **face embedding** generated from enrollment captures, serialized as text. It is not a raw face photograph. The [ML documentation](../ml/README.md) explains how embeddings are created and compared.

### Fingerprint

The database stores a `template_slot` number assigned to a user. The **AS608 stores the actual fingerprint template** in that slot. The backend's current allocator uses slots **1–162**, assuming that range is supported by the installed sensor.

For example, a row linking user `5` to slot `12` does **not** create a physical template in slot `12`. Seeded mappings and placeholder credentials must not be treated as proof of physical enrollment. The backend database and sensor must agree before matching, replacement, or deletion is tested.

### RFID

The database stores the full UID sent by the real card reader. Placeholder UIDs used during software testing are not interchangeable with a physically read UID.

## Access attempts and reenrollment

The `access_attempt` table records first-factor and second-factor results, the overall `SUCCESS`/`FAIL`/`EXPIRED` outcome, and associated timing. `SUCCESS` records a backend authentication decision; it does **not** establish that the door mechanically opened.

`pending_reenrollment` records the credential type the user must replace. During fingerprint replacement, the process is designed to retain old templates until replacement enrollment and old-slot deletion are confirmed. Because physical sensor changes and database writes are separate operations, interrupted operations can require reconciliation. See the [backend reenrollment explanation](../backend/README.md#reenrollment) and [hardware recovery proposal](../hardware/docs/FINGERPRINT_RECOVERY_PROPOSAL.md).

## Security and scope

PINs and administrator passwords are stored as hashes. Fingerprint templates remain on the AS608 rather than in PostgreSQL. Face embeddings and credential metadata are still sensitive and should be protected by database access controls and properly managed credentials.

The schema is intended for the current single-kiosk prototype. Shared multi-kiosk slot namespaces, audited schema migrations, and physical door-open events would require additional design.

## Related documentation

- [Backend and API](../backend/README.md)
- [Dashboard](../dashboard/README.md)
- [Face Recognition Evaluation](../ml/README.md)
- [Hardware and Firmware](../hardware/README.md)
