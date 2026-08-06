# WiFi Endpoints

## GET /wifi/scan
**Path:** `/wifi/scan` · **Method:** GET

**Purpose:** Scan for nearby WiFi access points. Blocks ~3.5 s on the device.

**Response** (`WifiScanResult` — see [messages.md#wifiscanresult](messages.md#wifiscanresult))
- CoAP codes: `2.05 Content` on success (AP list may be empty); `5.03` on scan failure.
- Clients should use a raised timeout (~10 s) for this endpoint.

## GET /wifi/status
**Path:** `/wifi/status` · **Method:** GET

**Purpose:** Read STA and SoftAP status.

**Response** (`WifiStatus` — see [messages.md#wifistatus](messages.md#wifistatus))
- CoAP codes: `2.05 Content` on success.

**Example request/response** (live device, 2026-08-06):
- Request: (empty)
- Response 2.05: `{sta_connected:true, sta_ip:"192.168.0.28", ap_ip:"192.168.4.1"}`

## POST /wifi/connect
**Path:** `/wifi/connect` · **Method:** POST

**Purpose:** Set the STA credentials and reconnect.

**Request** (`WifiConnectRequest` — see [messages.md#wificonnectrequest](messages.md#wificonnectrequest))
- `ssid`: network name.
- `password`: network password.

**Response** (`StatusResponse` — see [messages.md#statusresponse](messages.md#statusresponse))
- CoAP codes: `2.04 Changed` with `ok:true`; `5.03` on set-config failure; or the request may time out if the STA link drops and stops the CoAP server.

**Notes:** This endpoint disconnects/reconnects STA and may stop the CoAP server; place it last in a test sequence and tolerate a timeout. Empty credentials are accepted and recorded.
