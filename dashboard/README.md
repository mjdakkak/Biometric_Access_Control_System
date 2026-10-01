# Administrator Dashboard

The dashboard is the browser-based control panel for the **Biometric Access Control System**. It gives an administrator one place to create and manage users, request credential replacement, inspect authentication attempts, and change the administrator password. It was built with HTML, CSS, and JavaScript rather than a frontend framework.

The dashboard talks to FastAPI; it does **not** connect directly to PostgreSQL or communicate with the ESP32 sensors.

## Getting started

Set up the backend and database first, including the [first administrator account](../backend/README.md#4-create-the-first-administrator). Then start FastAPI from the repository root:

```powershell
python -m uvicorn backend.main:app --reload
```

Open [`http://127.0.0.1:8000/dashboard/login.html`](http://127.0.0.1:8000/dashboard/login.html). The hosted dashboard uses the same `/dashboard/login.html` path on the deployed API domain.

FastAPI serves the files in this folder using `StaticFiles`, so you do not need a separate frontend server for the normal setup. API requests use relative URLs such as `/admin/login` rather than a hardcoded localhost or Railway address.

## Pages

| Files | What they do |
| --- | --- |
| `login.html`, `login.js` | Login form, token storage, login errors, and redirect to the user list |
| `index.html`, `app.js` | Create and edit users, manage account status, and request reenrollment |
| `access.html`, `access.js` | Show recorded authentication attempts and results |
| `settings.html`, `settings.js` | Change the administrator password |
| `style.css` | Shared appearance and layout across the dashboard |

## Administrator login

The login form sends a username and password to `POST /admin/login`. FastAPI verifies the stored password hash and returns a JWT when the credentials are valid. The browser stores that token in local storage and sends it with protected requests:

```http
Authorization: Bearer <ADMIN_JWT>
```

The browser redirects to login when a token is missing. **Token validity is checked by the backend**, not by the page itself. Browser local storage is a practical choice for this prototype; it is not presented as a production-grade browser session design.

The login endpoint issues a JWT and therefore does not require an existing JWT. Administrator passwords and signing secrets are never placed in the dashboard files.

## User management

The main page displays registered employees and provides the administrative actions needed before and after enrollment. An administrator can create a record, edit the name, change the user's status, or request a credential replacement.

A new employee starts as `PENDING_ENROLLMENT`. They become `ACTIVE` only after completing RFID, face, and fingerprint enrollment on the physical kiosk. Setting a record to `INACTIVE` blocks normal authentication. The dashboard does not mark someone enrolled merely because their account exists.

### Reenrollment requests

An administrator can request replacement of `FACE`, `RFID`, or `FINGERPRINT`. The backend stores the pending request; the next successful ID/PIN entry at the kiosk routes that user into the corresponding reenrollment flow. The browser does not capture face images, write fingerprint templates, or delete sensor slots.

## Access-attempt history

The access page reads the `access_attempt` records exposed by FastAPI. Those records include the user, first and second factors, factor results, start and finish times, overall result, and a failure reason when available.

The overall result can be `SUCCESS`, `FAIL`, or `EXPIRED`. A `SUCCESS` entry indicates that the **backend approved authentication**; it is not an independent record of a physical door sensor or lock-open acknowledgment.

## Settings

The settings page allows an authenticated administrator to submit the current password and a replacement password. The backend verifies the current password and stores the hash of the new password. The dashboard itself does not store password hashes or modify the database directly.

## How the dashboard communicates with FastAPI

The interface uses ordinary `fetch()` requests. For example, the login request is made against a relative URL:

```javascript
fetch("/admin/login", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ username, password })
});
```

Using a shared origin keeps local and cloud paths consistent and avoids a separate frontend deployment. The administrator JWT and the kiosk's `X-Device-Key` serve **different purposes**: the browser uses the former, and the ESP32 uses the latter.

## Scope

This dashboard is for account administration and reviewing authentication history. It is not a remote-unlock console, and no dashboard-to-lock command channel is included in the current implementation.


