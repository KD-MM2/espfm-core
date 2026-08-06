# Curve Endpoints

## GET /curves
**Path:** `/curves` · **Method:** GET

**Purpose:** List all curves.

**Response** (`CurveList` — see [messages.md#curvelist](messages.md#curvelist))
- CoAP codes: `2.05 Content` on success.

**Example request/response** (live device, 2026-08-06):
- Request: (empty)
- Response 2.05: `CurveList{curves:[{id:0, name:"gpu-temp", points:5}]}`

## POST /curves
**Path:** `/curves` · **Method:** POST

**Purpose:** Create a curve.

**Request** (`CurveCreateRequest` — see [messages.md#curvecreaterequest](messages.md#curvecreaterequest))
- `name`: display name.
- `points`: 2-10 `CurvePoint`s, sorted by `temp_c` ascending.

**Response** (`CurveInfo` — see [messages.md#curveinfo](messages.md#curveinfo))
- CoAP codes: `2.01 Created` with the new `CurveInfo` (assigned `id`); `4.00 Bad Request` if undecodable or the upsert fails.

**Notes:** `POST /curves` upserts by id; a create without an id can land on slot 0 and overwrite an existing curve at id 0. Re-import via `POST /config` to restore the pre-existing curve.

## GET /curves/{id}
**Path:** `/curves/{0..15}` · **Method:** GET

**Purpose:** Read one curve.

**Response** (`CurveInfo` — see [messages.md#curveinfo](messages.md#curveinfo))
- CoAP codes: `2.05 Content`; `4.04 Not Found` if unallocated.

## PUT /curves/{id}
**Path:** `/curves/{0..15}` · **Method:** PUT

**Purpose:** Update a curve's name and/or points (full-replace upsert of points).

**Request** (`CurveUpdateRequest` — see [messages.md#curveupdaterequest](messages.md#curveupdaterequest))
- `id`: required.
- `name`: new name.
- `points`: full replacement list (2-10 points, sorted).

**Response** (`CurveInfo` — see [messages.md#curveinfo](messages.md#curveinfo))
- CoAP codes: `2.04 Changed` with the updated `CurveInfo`; `4.00` on decode failure or upsert failure (invalid point count or registry full).

**Notes:** `PUT /curves/{id}` upserts like `POST /curves`: an `id` that is not allocated is created at the first free slot (the response carries the actual slot id). There is no `4.04` — the handler never returns Not Found.

## DELETE /curves/{id}
**Path:** `/curves/{0..15}` · **Method:** DELETE

**Purpose:** Remove a curve.

**Response** (`StatusResponse` — see [messages.md#statusresponse](messages.md#statusresponse))
- CoAP codes: `2.02 Deleted` with `ok:true`; `4.04 Not Found`.
