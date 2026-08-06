# System Endpoints

## GET /system/info
**Path:** `/system/info` · **Method:** GET · **Content-Format:** application/octet-stream (protobuf)

**Purpose:** Read system identity, uptime, heap, and entity counts.

**Response** (`SystemInfo` — see [messages.md#systeminfo](messages.md#systeminfo))
- CoAP codes: `2.05 Content` on success; `4.04 Not Found` on path mismatch
- No request payload

**Example request/response** (live device espfm-c425, 2026-08-06):
- Request: (empty)
- Response 2.05: `{version:"1.0.0", uptime_s:676, heap_free:152080, fan_count:1, source_count:1, curve_count:1, schedule_count:0, hostname:"espfm-c425"}`

## PUT /system/hostname
**Path:** `/system/hostname` · **Method:** PUT

**Purpose:** Set the device mDNS hostname. Persists to NVS.

**Request** (`HostnameRequest` — see [messages.md#hostnamerequest](messages.md#hostnamerequest))
- `hostname`: new hostname, without `.local`.

**Response** (`StatusResponse` — see [messages.md#statusresponse](messages.md#statusresponse))
- CoAP codes: `2.04 Changed` on success; `4.00` if undecodable or mDNS set fails; `4.04` if path mismatched.

**Notes:** Restore the original hostname after testing; the value persists across reboots.

## POST /system/reboot
**Path:** `/system/reboot` · **Method:** POST

**Purpose:** Reboot the device after ~2 seconds.

**Response** (`StatusResponse` — see [messages.md#statusresponse](messages.md#statusresponse))
- CoAP codes: `2.04 Changed` on success (reboot scheduled); `5.03 Service Unavailable` with `error_msg="reboot pending"` if a reboot is already pending.

**Example request/response** (live device, 2026-08-06 — reboot already pending after a config import):
- Request: (empty)
- Response 5.03: `{ok:false, error_code:0, error_msg:"reboot pending"}`

**Notes:** The device restarts ~2 s after the 2.04; clients must tolerate the connection drop and re-probe. A 5.03 "reboot pending" is a valid outcome to record and retry after the pending reboot completes.
