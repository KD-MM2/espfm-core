# Source Endpoints

## GET /sources
**Path:** `/sources` · **Method:** GET

**Purpose:** List all sources.

**Response** (`SourceList` — see [messages.md#sourcelist](messages.md#sourcelist))
- CoAP codes: `2.05 Content` on success.

**Example request/response** (live device, 2026-08-06):
- Request: (empty)
- Response 2.05: `SourceList{sources:[{id:0, name:"gpu-manual", type:2, status:1, temp_c:20.0, gpio:255}]}`

## POST /sources
**Path:** `/sources` · **Method:** POST

**Purpose:** Create a source (NTC, DS18B20, or manual).

**Request** (`SourceCreateRequest` — see [messages.md#sourcecreaterequest](messages.md#sourcecreaterequest))
- `type`: required.
- `gpio`: `255` = none; required for NTC, ignored otherwise.
- `ds18b20_rom_code`: required for DS18B20.
- `name`: display name.

**Response** (`SourceInfo` — see [messages.md#sourceinfo](messages.md#sourceinfo))
- CoAP codes: `2.01 Created` with the new `SourceInfo` (assigned `id`); `4.00 Bad Request` with `StatusResponse` on add failure.

**Example request/response** (live device, 2026-08-06):
- Request: `SourceCreateRequest{type:2, name:"test-source", gpio:255}`
- Response 2.01: `{id:1, name:"test-source", type:2, gpio:255}`

## POST /sources/temp
**Path:** `/sources/temp` · **Method:** POST

**Purpose:** Set the temperature of a manual source.

**Request** (`ManualTempRequest` — see [messages.md#manualtemprequest](messages.md#manualtemprequest))
- `id`: a manual source id.
- `temp_c`: temperature in °C, range -40..125.

**Response** (`StatusResponse` — see [messages.md#statusresponse](messages.md#statusresponse))
- CoAP codes: `2.04 Changed` with `ok:true`; `4.04 Not Found` if the source doesn't exist; `4.00` on failure.

**Example request/response** (live device, 2026-08-06):
- Request: `ManualTempRequest{id:0, temp_c:20.0}`
- Response 2.04: `{ok:true}`

## GET /sources/{id}
**Path:** `/sources/{0..7}` · **Method:** GET

**Purpose:** Read one source.

**Response** (`SourceInfo` — see [messages.md#sourceinfo](messages.md#sourceinfo))
- CoAP codes: `2.05 Content`; `4.04 Not Found` if unallocated.

## PUT /sources/{id}
**Path:** `/sources/{0..7}` · **Method:** PUT

**Purpose:** Rename a source.

**Request** (`SourceUpdateRequest` — see [messages.md#sourceupdaterequest](messages.md#sourceupdaterequest))
- `id`, `name`: required.

**Response** (`SourceInfo` — see [messages.md#sourceinfo](messages.md#sourceinfo))
- CoAP codes: `2.04 Changed`; `4.04 Not Found`; `4.00` on failure.

## DELETE /sources/{id}
**Path:** `/sources/{0..7}` · **Method:** DELETE

**Purpose:** Remove a source.

**Response** (`StatusResponse` — see [messages.md#statusresponse](messages.md#statusresponse))
- CoAP codes: `2.02 Deleted` with `ok:true`; `4.04 Not Found`.
