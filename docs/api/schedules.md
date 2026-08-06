# Schedule Endpoints

## GET /schedules
**Path:** `/schedules` · **Method:** GET

**Purpose:** List all schedules.

**Response** (`ScheduleList` — see [messages.md#schedulelist](messages.md#schedulelist))
- CoAP codes: `2.05 Content` on success (may be empty).

**Example request/response** (live device, 2026-08-06 — no schedules configured):
- Request: (empty)
- Response 2.05: `ScheduleList{schedules:[]}`

## POST /schedules
**Path:** `/schedules` · **Method:** POST

**Purpose:** Create a schedule.

**Request** (`ScheduleCreateRequest` — see [messages.md#schedulecreaterequest](messages.md#schedulecreaterequest))
- `fan_id`: bound fan (0-7).
- `duty`: 0-100.
- `start_min`/`end_min`: 0-1439 minutes since midnight.
- `enabled`: rule active.
- `name`: optional display name.

**Response** (`ScheduleInfo` — see [messages.md#scheduleinfo](messages.md#scheduleinfo))
- CoAP codes: `2.01 Created` with the new `ScheduleInfo` (assigned `id`); `4.00 Bad Request` on add failure.

## GET /schedules/{id}
**Path:** `/schedules/{0..7}` · **Method:** GET

**Purpose:** Read one schedule.

**Response** (`ScheduleInfo` — see [messages.md#scheduleinfo](messages.md#scheduleinfo))
- CoAP codes: `2.05 Content`; `4.04 Not Found` if unallocated.

## PUT /schedules/{id}
**Path:** `/schedules/{0..7}` · **Method:** PUT

**Purpose:** Update one or more schedule fields (partial update).

**Request** (`ScheduleUpdateRequest` — see [messages.md#scheduleupdaterequest](messages.md#scheduleupdaterequest))
- `id`: required.
- All other fields optional.

**Response** (`ScheduleInfo` — see [messages.md#scheduleinfo](messages.md#scheduleinfo))
- CoAP codes: `2.04 Changed`; `4.04 Not Found`; `4.00` on failure.

## DELETE /schedules/{id}
**Path:** `/schedules/{0..7}` · **Method:** DELETE

**Purpose:** Remove a schedule.

**Response** (`StatusResponse` — see [messages.md#statusresponse](messages.md#statusresponse))
- CoAP codes: `2.02 Deleted` with `ok:true`; `4.04 Not Found`.
