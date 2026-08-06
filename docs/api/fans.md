# Fan Endpoints

## GET /fans
**Path:** `/fans` · **Method:** GET

**Purpose:** List all fans.

**Response** (`FanList` — see [messages.md#fanlist](messages.md#fanlist))
- CoAP codes: `2.05 Content` on success.

**Example request/response** (live device, 2026-08-06):
- Request: (empty)
- Response 2.05: `FanList{fans:[{id:0, name:"gpu-fan-1", mode:1, duty:20, rpm:13290, pwm_gpio:22, tach_gpio:23, enabled:true}]}`

## POST /fans
**Path:** `/fans` · **Method:** POST

**Purpose:** Create a fan.

**Request** (`FanCreateRequest` — see [messages.md#fancreaterequest](messages.md#fancreaterequest))
- `pwm_gpio`: required, 0-48; must not be a reserved or already-claimed pin.
- `tach_gpio`: optional, `255` = none.
- All other fields optional.

**Response** (`FanInfo` — see [messages.md#faninfo](messages.md#faninfo))
- CoAP codes: `2.01 Created` with the new `FanInfo` (assigned `id`); `4.00 Bad Request` with `StatusResponse` on GPIO claim/constraint failure.

**Example request/response** (live device, 2026-08-06):
- Request: `FanCreateRequest{pwm_gpio:2, tach_gpio:4, name:"test-fan"}`
- Response 2.01: `{id:1, name:"test-fan", pwm_gpio:2, tach_gpio:4, enabled:true}`
- Failure 4.00: `{ok:false, error_code:258, error_msg:"GPIO 1 is reserved"}`

## GET /fans/{id}
**Path:** `/fans/{0..7}` · **Method:** GET

**Purpose:** Read one fan.

**Response** (`FanInfo` — see [messages.md#faninfo](messages.md#faninfo))
- CoAP codes: `2.05 Content`; `4.04 Not Found` if the id is unallocated.

**Example request/response** (live device, 2026-08-06):
- Request: (empty)
- Response 2.05: `{id:0, name:"gpu-fan-1", mode:1, duty:20, rpm:13290, pwm_gpio:22, tach_gpio:23}`

## PUT /fans/{id}
**Path:** `/fans/{0..7}` · **Method:** PUT

**Purpose:** Update one or more fan fields (partial update — omitted optional fields are unchanged).

**Request** (`FanUpdateRequest` — see [messages.md#fanupdaterequest](messages.md#fanupdaterequest))
- `id`: required.
- All other fields optional.

**Response** (`FanInfo` — see [messages.md#faninfo](messages.md#faninfo))
- CoAP codes: `2.04 Changed` with the updated `FanInfo`; `4.04 Not Found`; `4.00 Bad Request` with `StatusResponse` on a GPIO-swap/claim failure.

**Example request/response** (live device, 2026-08-06):
- Request: `FanUpdateRequest{id:0, duty:40}`
- Response 2.04: `{id:0, duty:40, ...}`

## DELETE /fans/{id}
**Path:** `/fans/{0..7}` · **Method:** DELETE

**Purpose:** Remove a fan (releases its PWM/tach GPIOs).

**Response** (`StatusResponse` — see [messages.md#statusresponse](messages.md#statusresponse))
- CoAP codes: `2.02 Deleted` with `ok:true`; `4.04 Not Found` if unallocated.

**Example request/response** (live device, 2026-08-06):
- Request: (empty)
- Response 2.02: `{ok:true, error_code:0, error_msg:""}`
