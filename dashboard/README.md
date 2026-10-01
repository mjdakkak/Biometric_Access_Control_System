# Administrator Dashboard

The administrator dashboard is a lightweight **HTML, CSS, and JavaScript** interface for the Biometric Access Control System. It uses FastAPI's administrator endpoints to manage users and credentials, review authentication attempts, and update the administrator password. It does not communicate directly with sensors or PostgreSQL.

## Overview

```text
Administrator -> Browser dashboard -> FastAPI admin API -> PostgreSQL
```

The dashboard and API are served from the **same domain** in the deployed project. JavaScript calls use relative URLs (for example, `/admin/login`) instead of hardcoded local or Railway addresses.

## Getting started

From the repository root, follow the [backend setup](../backend/README.md#getting-started) to initialize PostgreSQL, configure `.env`, and start FastAPI:

```bash
python -m uvicorn backend.main:app --reload
```

Open the dashboard at:

`http://127.0.0.1:8000/dashboard/login.html`

A valid administrator account must already exist in the database. The schema does **not** create a default administrator. Administrator provisioning and credentials are backend responsibilities; do not put passwords or tokens in the dashboard source code.

## Pages and files

| File | Purpose |
| --- | --- |
| `login.html` / `login.js` | Administrator login and token handling |
| `index.html` / `app.js` | User list, user creation/editing, status changes, and reenrollment requests |
| `access.html` / `access.js` | Authentication-attempt history |
| `settings.html` / `settings.js` | Administrator password change |
| `style.css` | Shared styling across pages |

## Administrator login

The login form sends credentials to `POST /admin/login`. The backend checks the stored password hash and, on success, returns a **JWT** (JSON Web Token). The browser stores the token in local storage and sends it with protected requests:

```http
Authorization: Bearer <ADMIN_JWT>
```

Pages check for a token and redirect to login when none is available. The backend—not the browser—is responsible for deciding whether a token is valid. A token stored in local storage is a practical choice for this prototype, not a hardened production session-management design.

## User management

The main dashboard allows an administrator to create and edit user records, view status, change status, and request credential reenrollment. New accounts begin as `PENDING_ENROLLMENT`; the physical kiosk and backend must complete enrollment before the user becomes `ACTIVE`.

The three account statuses are:

- `PENDING_ENROLLMENT`: account created, physical credential enrollment incomplete.
- `ACTIVE`: enrolled and eligible for normal authentication, unless reenrollment is pending.
- `INACTIVE`: authentication blocked.

An administrator can request replacement of `FACE`, `RFID`, or `FINGERPRINT`. The dashboard records the request through the API; the next ID/PIN interaction at the kiosk starts the corresponding reenrollment flow. The dashboard itself does not capture faces or write AS608 templates.

## Access attempts

The access page retrieves backend records showing the user, authentication factors, results, timestamps, and failure information when available. Overall outcomes include `SUCCESS`, `FAIL`, and `EXPIRED`.

A `SUCCESS` record means **authentication was approved by the backend**. It should not be interpreted as independent confirmation that the physical door opened.

## Settings and password changes

The settings page sends the current and replacement administrator passwords to the protected backend password-change endpoint. The backend verifies the current password and saves a new hash. The dashboard does not store plaintext passwords in its source code.

## API communication and deployment

The interface uses `fetch()` and relative endpoint paths, such as:

```javascript
fetch("/admin/login", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ username, password })
});
```

FastAPI mounts the dashboard folder at `/dashboard`, so the deployed entry point is `<API_BASE_URL>/dashboard/login.html`. Keeping the dashboard and API on one origin avoids separate frontend-hosting configuration for this prototype.

## Security and scope

The dashboard uses **administrator JWTs**; the ESP32 uses the separate `X-Device-Key` header. Neither credential belongs in public source code. Because the current token is stored in browser local storage, a production deployment would need a broader browser-security and session-management review.

The dashboard supports user/credential administration and log review. **Remote unlock is not implemented** as a dashboard command in the current scope.

## Related documentation

- [Backend](../backend/README.md)
- [Database](../database/README.md)
- [Face Recognition Evaluation](../ml/README.md)
- [Hardware and Firmware](../hardware/README.md)
