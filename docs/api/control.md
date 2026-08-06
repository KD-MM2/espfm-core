# Control Endpoints

## GET /control
**Path:** `/control` · **Method:** GET

**Purpose:** Read the control-loop tunables.

**Response** (`ControlConfig` — see [messages.md#controlconfig](messages.md#controlconfig))
- CoAP codes: `2.05 Content` on success; `5.03 Service Unavailable` if the control loop is unset; `5.00` on internal error.

**Example request/response** (live device, 2026-08-06):
- Request: (empty)
- Response 2.05: `{hysteresis:3, ramp_up:10, ramp_down:3, failsafe_policy:2, safe_duty:50}`

## PUT /control
**Path:** `/control` · **Method:** PUT

**Purpose:** Update control-loop tunables (partial update — omitted fields unchanged).

**Request** (`ControlConfig` — see [messages.md#controlconfig](messages.md#controlconfig))
- All fields optional, ranges: `hysteresis`/`ramp_up`/`ramp_down`/`safe_duty` 0-100; `failsafe_policy` 0-3.

**Response** (`StatusResponse` — see [messages.md#statusresponse](messages.md#statusresponse))
- CoAP codes: `2.04 Changed` with `ok:true`; `4.00 Bad Request` with `StatusResponse` on a range violation or decode failure; `5.03` if the control loop is unset.

**Example request/response** (live device, 2026-08-06):
- Request: `ControlConfig{hysteresis:3, ramp_up:10, ramp_down:3, failsafe_policy:2, safe_duty:50}`
- Response 2.04: `{ok:true, error_code:0, error_msg:""}`
- Out-of-range 4.00: `ControlConfig{hysteresis:150, ...}` → `{ok:false, error_code:258, error_msg:"hysteresis out of range"}`
