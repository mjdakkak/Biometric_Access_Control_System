# PostgreSQL Database

PostgreSQL holds the persistent records for the **Biometric Access Control System**: employee accounts, credential information, administrator accounts, pending reenrollment requests, and authentication history. FastAPI is responsible for querying and updating these records. The ESP32 and the dashboard do not connect to PostgreSQL directly.

One important design choice is that **fingerprint templates are not stored in the database**. They are stored on the physical AS608 sensor. PostgreSQL records which sensor slots belong to each user.

## Files

- [`schema.sql`](schema.sql) — tables, sequence, keys, relationships, and constraints.
- [`biometric_access_erd.drawio`](biometric_access_erd.drawio) — editable entity-relationship diagram.

The editable ERD is included in the repository; no PNG export is required to inspect or modify the database design.

## Setup

Create an empty PostgreSQL database and apply the schema from the repository root:

```bash
psql -h localhost -U YOUR_DB_USER -d YOUR_DB_NAME -f database/schema.sql
```

Set `DB_HOST`, `DB_PORT`, `DB_NAME`, `DB_USER`, and `DB_PASSWORD` in the backend's local `.env` file. The available variables are documented in [`.env.example`](../.env.example), and the full application startup procedure is in the [backend README](../backend/README.md#getting-started).

The schema creates an **empty database**. It does not seed users, create the first administrator, import records from another database, or create fingerprint templates on the physical sensor. To provision an administrator after applying the schema, follow [Create the first administrator](../backend/README.md#4-create-the-first-administrator).

A local PostgreSQL instance and a Railway-hosted instance are separate databases unless records are explicitly migrated between them.

## Tables and relationships

| Table | Stored information |
| --- | --- |
| `users` | Internal ID, employee ID, name, account status, and creation time |
| `pin_credential` | User's hashed four-digit PIN |
| `rfid_credential` | User's full card UID and credential status |
| `face_credential` | Serialized face embedding and credential status |
| `fingerprint_credential` | User-to-AS608-template-slot mapping and credential status |
| `access_attempt` | Authentication factors, results, timestamps, and failure reason |
| `pending_reenrollment` | A user's pending `FACE`, `RFID`, or `FINGERPRINT` replacement request |
| `admin_user` | Administrator username and password hash |

The schema also defines `employee_id_seq` for generating employee IDs.

### User identity

`users.id` is the internal primary key referenced by credential and access-attempt records. `users.employee_id` is the unique identifier shown at the kiosk and formatted to five digits by the application.

Users have one of three states:

- `PENDING_ENROLLMENT`: account exists, but physical enrollment has not finished.
- `ACTIVE`: the account has completed enrollment and can authenticate unless reenrollment is pending.
- `INACTIVE`: normal authentication is blocked.

A user has one PIN record and can have several historical access attempts. Credential tables preserve the association between a user and each enrolled factor. Some credential replacement steps temporarily involve old and new records; the exact number of active records is managed by the application and should not be assumed to be a database constraint unless the SQL enforces it.

### Face credentials

The database stores a **512-dimensional face embedding**, serialized as text, rather than a raw enrollment photograph. The backend creates this representation from the accepted enrollment captures. Face verification compares a new embedding against the expected user's stored representation. See the [ML evaluation](../ml/README.md) for the distance metric, experiments, and threshold.

### RFID credentials

The RFID table stores the **full UID** provided by the physical card reader. Its unique constraint prevents two credential records from using the same UID. Placeholder values inserted during software tests do not represent real cards and should be removed or reconciled before hardware enrollment.

### Fingerprint credentials

The backend's allocator uses AS608 slots **1–162** for this kiosk. `fingerprint_credential` records a user ID and an assigned `template_slot`; the actual fingerprint template remains on the sensor.

For example, a row associating user `5` with slot `12` means the backend **expects** the user's physical template to be present in slot `12`. Inserting that row does not write anything to the sensor. This is why the physical sensor's contents and the database mappings must be checked together before replacement or deletion.

## Access attempts

An access-attempt record tracks the first factor (`RFID` or `PIN`), the requested second factor (`FACE` or `FINGERPRINT`), factor results, timestamps, and the final `SUCCESS`, `FAIL`, or `EXPIRED` result.

A successful access attempt means the backend accepted the two factors. It is **not proof that the door physically opened**. Recording a lock acknowledgment or door-sensor event would require a separate hardware signal and corresponding data model.

## Pending reenrollment and physical state

The `pending_reenrollment` table records the credential type that must be replaced. When a user with a pending request enters their ID and PIN, the backend routes them to reenrollment rather than normal authentication.

Fingerprint replacement needs particular care because the database and the sensor cannot be updated in one atomic transaction. The intended sequence enrolls replacements before requesting old-template deletion, then waits for physical deletion confirmation before removing old database mappings. Unexpected power loss or network interruption can still require reconciliation. The [backend reenrollment section](../backend/README.md#credential-reenrollment) explains the normal flow, and the [hardware recovery proposal](../hardware/docs/FINGERPRINT_RECOVERY_PROPOSAL.md) covers interrupted operations. A recovery proposal is not evidence that automatic recovery is active.

## Constraints and security

The schema uses primary and foreign keys to connect records to users, along with uniqueness constraints for employee IDs, RFID UIDs, and fingerprint slots. Check constraints limit allowed user statuses, authentication factors, outcomes, and reenrollment types.

PINs and administrator passwords are stored as hashes rather than plaintext. Face embeddings and credential mappings are still sensitive information and should be handled as such. Database passwords belong in environment variables, never in the public repository.

