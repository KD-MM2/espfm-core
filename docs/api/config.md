# Config Endpoints

## GET /config
**Path:** `/config` · **Method:** GET

**Purpose:** Export the full device configuration (all fans, sources, curves, schedules).

**Response** (`ConfigFile` — see [messages.md#configfile](messages.md#configfile))
- CoAP codes: `2.05 Content` on success; `5.00` on export failure.
- May use Block2 when the encoded config exceeds one datagram.

**Example request/response** (live device, 2026-08-06):
- Request: (empty)
- Response 2.05: `ConfigFile{version:"3.0", fans:1, sources:1, curves:1, schedules:0}`

## POST /config
**Path:** `/config` · **Method:** POST

**Purpose:** Import a full configuration: validates the whole `ConfigFile`, clears all registries, applies, force-persists, then reboots ~2 s later.

**Request** (`ConfigFile` — see [messages.md#configfile](messages.md#configfile))
- Must be a valid full config: per-registry capacity, GPIO validity (reserved/duplicate/DS18B20-bus pins rejected), fan cross-references to existing source/curve/schedule, curve point count/order, schedule range/duty and fan binding.

**Response** (`StatusResponse` — see [messages.md#statusresponse](messages.md#statusresponse))
- CoAP codes: `2.04 Changed` with `ok:true`, then a ~2 s reboot; `4.00 Bad Request` on validation failure (zero mutation, no reboot) with `error_msg`; `5.00` on persist failure.

**Notes:** The device reboots ~2 s after the 2.04 — clients must tolerate the connection drop and re-probe. On apply failure the registries are re-cleared and the device reboots to reload the previous config.

**Example request/response** (live device, 2026-08-06):
- Request: `ConfigFile{version:"3.0", fans:1, sources:1, curves:1, schedules:0}`
- Response 2.04: `{ok:true, error_code:0, error_msg:""}`; device returned after reboot.
