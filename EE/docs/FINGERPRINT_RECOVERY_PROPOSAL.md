# Proposed fingerprint recovery contract — not an existing API

This is the missing server-side agreement. The normal kiosk endpoints and all
existing eight C++ hook signatures remain unchanged. Endpoint paths are deliberately
not invented here; agree them before writing the HTTP adapter.

## What is implemented locally

`fingerprint_recovery.h` exposes these queue-based calls:

```cpp
bool requestFingerprintRecoveryInspection(uint32_t requestId);
bool pollFingerprintRecoveryReport(FingerprintRecoveryReport &report);
bool submitFingerprintRecoveryPlan(const FingerprintRecoveryPlan &plan);
bool pollFingerprintRecoveryReceipt(FingerprintRecoveryReceipt &receipt);
bool acknowledgeFingerprintRecovery(const FingerprintRecoveryAcknowledgement &ack);
void cancelFingerprintRecovery();
```

All bool returns indicate queued, not approved or completed. The existing
fingerprint worker is the sole UART/NVS owner. The future maintenance coordinator
must pause/cancel ordinary kiosk sessions and submit a `FingerprintOperation::None`
command first. Inspection of an active capture returns Busy. There is no new
Nextion command or unauthenticated serial console which performs these operations.

## 1. Inspect without changing the sensor or clearing the record

A report contains its local request ID, an inspection ticket, a SHA-256 `journalId`,
the original session/operation/exact affected and protected slots, sensor capacity,
and each relevant slot's observed state (absent, readable, unreadable, unknown).
`journalId` hashes the original persistent record; `ticket` is a per-inspection
nonce. Neither is a password, signature, attestation, nor proof of authorization.
The read-only inspection does open Preferences and performs UART read commands;
it does not store or delete sensor templates or clear pending intent.

The report remains quarantined when the record is invalid/unknown-version,
communication fails, a slot lies outside actual capacity, or evidence is missing.
The AS608 default UART address is not a unique sensor identity. Backend device
credentials and a controlled physical sensor assignment must bind the report to
one device and sensor inventory. If hardware was swapped or changed externally,
occupancy/readability alone is insufficient: return Hold for controlled maintenance.

## 2. Server chooses and durably records an authorized plan

Backend must authenticate the device, authorize maintenance, cross-check its
persistent database, reserve the affected slots, and associate the decision with
that device, journalId, and current inspection ticket. It must not infer database
commit from sensor occupancy. Keep slots reserved until reconciliation finishes;
normal enrollment and a second maintainer must not race with this repair.

Supported actions (C++ enum values; JSON spelling to be agreed):

| Action | Preconditions / effect |
|---|---|
| Hold | No repair; retain pending record. |
| KeepStoredTemplate | Original operation Enroll, target readable, protected slots intact; backend explicitly confirms the target belongs to that intended enrollment and should be retained. No sensor write. |
| DiscardUncommittedTemplate | Original operation Enroll; backend explicitly confirms target is not referenced by a valid committed credential and authorizes removing it. Deletes only that original target if present; absent is an idempotent no-op. |
| FinishOldDeletion | Original operation DeleteOld; both replacement slots remain readable, backend confirms replacements are committed and authorizes only the original old slots. Delete only remaining old templates, never replacements. |

`FingerprintRecoveryPlan` has no arbitrary slot list. The device takes the target
and protected slots from its validated persistent journal, not from an editable
screen message. It checks ticket/hash/expiry, re-reads inventory and aborts when
observations changed. Plans expire 60 seconds after inspection; obtain a fresh
report rather than silently extending an old decision.

`KIOSK_ENABLE_FP_RECOVERY_APPLY` defaults to 0, so plan application and final
clearing are blocked until the trusted adapter is ready. Setting this to 1 is
not itself authentication; caller verification is still required.

## 3. Device applies, verifies, and retains intent

Only the original uncommitted target or recorded old slots can be deleted.
Protected slots are never deletion targets. Keep operations do not rewrite an
existing template. No method empties the fingerprint database.

The original NVS record stays present throughout. `AppliedAwaitingBackend` means
the requested physical post-state was verified, not that NVS was cleared. It
returns a fresh receipt ticket and the same journal/session identity. Partial
failure, cancellation, lost sensor ACK, full queues, or power loss retain intent.
After reboot, start with a fresh inspection and ask the server again; do not replay
an old RAM ticket. Already-absent authorized deletion targets require no delete.

## 4. Backend commits the reconciliation, then acknowledges

Backend verifies the receipt belongs to its durable plan, commits the reconciled
mappings/audit/status in its database, and returns a device-bound acknowledgement
of that exact receipt. The client verifies authentication/correlation before calling
`acknowledgeFingerprintRecovery`. Worker rechecks the physical post-state before
removing only the pending record. NVS removal failure is reported, not hidden.

Ordinary request_id echoing is NOT sufficient here: the server must persist repair
state/idempotency using the device and journal identity, handle retries after lost
responses, and not require a now-expired in-RAM authentication session to recover
an uncertain durable enrollment. The normal finalized enrollment/deletion endpoints
cannot be assumed retry-safe without your explicit server implementation.

## Limits and open integration work

- Device engine implemented and tested with simulated sensor/storage responses.
- Actual AS608 inspection, codec/inference, timing and ESP32 build are untested.
- Server recovery decision routes, durable reservations/idempotency, authenticated
  transport, HTTP client and maintenance orchestration are NOT implemented here.
- No automatic startup repair is performed. A pending record remains blocked.
- Invalid/unknown journal bytes are not erasable through this protocol. Controlled
  authenticated maintenance/migration is a separate deliberate procedure.
- The worker cannot prove template ownership by reading the occupancy table; this
  contract assumes no out-of-band sensor mutation and a stable physical assignment.
- No 'authorized=true' boolean, hash, ticket, or local serial input substitutes for
  authenticated backend authorization.
