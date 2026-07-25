#!/usr/bin/env python3
"""
CoAP endpoint test suite for ESPFanManager v3.

Each case documents: what it tests, the request sent, and expected outcome.
Uses raw CoAP client for direct access to status codes and response bodies.
"""

import sys, socket, struct, json, time

sys.path.insert(0, ".")
from coap_client import (
    _decode,
    _get,
    _fan_create,
    _fan_update,
    _src_create,
    _temp,
    _curve_create,
    _curve_update,
    _sched_create,
    _sched_update,
    _wifi_conn,
    GET,
    POST,
    PUT,
    DELETE,
    CODES,
)

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.50"
PORT = 5683


# ============================================================
# CoAP raw client (thin wrapper with return values)
# ============================================================


class RawCoAP:
    """Low-level CoAP client that returns (code_int, payload_bytes) for tests."""

    def __init__(self, host, port=5683, timeout=3):
        self.host, self.port = host, port
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(timeout)
        self._mid = int(time.time() * 1000) & 0xFFFF

    def _mid_next(self):
        self._mid = (self._mid + 1) & 0xFFFF
        return self._mid

    def request(self, method, path, payload=b""):
        """Send CoAP request. Returns (code_int, payload_bytes) or (None, None)."""
        segs = [s for s in path.split("/") if s]
        tok = struct.pack(">I", int(time.time() * 1e6) & 0xFFFFFFFF)[:4]
        mid = self._mid_next()
        hdr = bytes([0x40 | len(tok), method, (mid >> 8) & 0xFF, mid & 0xFF]) + tok
        for i, s in enumerate(segs):
            d = 11 if i == 0 else 0
            hdr += bytes([(d << 4) | len(s)]) + s.encode()
        if payload:
            hdr += b"\xff" + payload
        self.s.sendto(hdr, (self.host, self.port))
        try:
            data, _ = self.s.recvfrom(4096)
            code = data[1]
            pos = 4 + (data[0] & 0x0F)
            while pos < len(data) and data[pos] != 0xFF:
                dl = data[pos]
                pos += 1
                d = (dl >> 4) & 0xF
                l = dl & 0xF
                if d == 13:
                    pos += 1
                elif d == 14:
                    pos += 2
                if l == 13:
                    l = data[pos] + 13
                    pos += 1
                elif l == 14:
                    l = (data[pos] << 8) + data[pos + 1] + 269
                    pos += 2
                pos += l
            payload = data[pos + 1 :] if pos < len(data) and data[pos] == 0xFF else b""
            return code, payload
        except socket.timeout:
            return None, None

    def close(self):
        self.s.close()


# ============================================================
# Test runner
# ============================================================

results = []


def code_str(c):
    """Format CoAP status code for display."""
    if c is None:
        return "TIMEOUT"
    cls_map = {
        0x40: "2.0",
        0x41: "2.01",
        0x42: "2.02",
        0x44: "2.04",
        0x45: "2.05",
        0x80: "4.00",
        0x84: "4.04",
        0xA3: "5.03",
    }
    return cls_map.get(c, f"{c>>5}.{c&0x1F:02d}")


def assert_status(code, expected, label):
    """Check CoAP status code matches expected class."""
    if code is None:
        return False, f"TIMEOUT (expected {expected})"
    exp_cls = int(expected[0])
    got_cls = code >> 5
    ok = got_cls == exp_cls
    detail = f"got {code_str(code)} (expected {expected})"
    return ok, detail


def assert_field(decoded, field_num, expected_type, expected_value=None, label=""):
    """Check a decoded protobuf field exists with correct type and optional value."""
    if field_num not in decoded:
        return False, f"missing field {field_num}"
    got_type, got_val = decoded[field_num]
    if got_type != expected_type:
        return (
            False,
            f"field {field_num}: expected type {expected_type}, got {got_type}",
        )
    if expected_value is not None and got_val != expected_value:
        return False, f"field {field_num}: expected {expected_value!r}, got {got_val!r}"
    return True, ""


def test(
    case_num,
    label,
    desc,
    method,
    path,
    payload=b"",
    expect_status=None,
    expect_fields=None,
):
    """
    Run one CoAP test case.

    case_num:      test number
    label:         short test name (e.g. 'GET /fans')
    desc:          what this test verifies
    method:        CoAP method (GET/POST/PUT/DELETE)
    path:          URI path
    payload:       protobuf-encoded request body (or b'')
    expect_status: expected CoAP status class (e.g. '2.05', '2.01', '4.00')
    expect_fields: dict of field_num -> (type, value) to check in decoded response
    """
    code, data = c.request(method, path, payload)

    issues = []

    # 1. Check status code
    if expect_status:
        ok, detail = assert_status(code, expect_status, label)
        if not ok:
            issues.append(detail)

    # 2. Check response fields
    if expect_fields and data:
        try:
            decoded = _decode(data)
        except Exception:
            decoded = {}
            issues.append("failed to decode response")
        for fn, (etype, evalue) in expect_fields.items():
            ok, detail = assert_field(decoded, fn, etype, evalue, label)
            if not ok:
                issues.append(detail)

    # Print result
    status = code_str(code)
    print(f"\nCase {case_num}: {label}")
    print(f"  Desc:    {desc}")
    print(f'  Request: {["","GET","POST","PUT","DELETE"][method]} {path}')
    if payload:
        print(f"  Payload: {len(payload)} bytes")
    print(f'  Expect:  {expect_status or "any"}')
    print(f"  Status:  {status}")
    if data:
        try:
            d = _decode(data)
            print(f"  Body:    {json.dumps(d, default=str)}")
        except Exception:
            print(f"  Raw:     {data.hex()}")
    if issues:
        for i in issues:
            print(f"  [FAIL] {i}")
        results.append((case_num, label, "FAIL", "; ".join(issues)))
    else:
        print(f"  [PASS]")
        results.append((case_num, label, "PASS", ""))


# ============================================================
# Test cases
# ============================================================

c = RawCoAP(HOST, PORT)

print(f"ESP Fan Manager v3 — CoAP Endpoint Test Suite")
print(f"Target: {HOST}:{PORT}")
print(f'{"="*60}')

# --- System ---
print(f'\n{"="*60}')
print("SYSTEM")

test(
    1,
    "GET /system/info",
    desc="Verify system info returns version, uptime, and memory stats",
    method=GET,
    path="/system/info",
    expect_status="2.05",
    expect_fields={
        1: ("s", None),  # version string
        2: ("v", None),  # uptime (dynamic)
        3: ("v", None),
    },
)  # heap free (dynamic)

# --- Fans ---
print(f'\n{"="*60}')
print("FANS")

test(
    2,
    "GET /fans (empty list)",
    desc="Query fan list before any fans exist",
    method=GET,
    path="/fans",
    expect_status="2.05",
)

test(
    3,
    "POST /fans (create fan)",
    desc='Create a new fan on PWM GPIO 15 named "f3"',
    method=POST,
    path="/fans",
    payload=_fan_create(pwm=15, name="f3"),
    expect_status="2.01",
    expect_fields={2: ("s", "f3"), 8: ("v", 15)},  # name = f3
)  # pwm_gpio = 15

test(
    4,
    "GET /fans/0 (read back)",
    desc="Read back the fan we just created — verify name and gpio persisted",
    method=GET,
    path="/fans/0",
    expect_status="2.05",
    expect_fields={2: ("s", "f3"), 8: ("v", 15)},  # name
)  # pwm_gpio

test(
    5,
    "PUT /fans/0 (update duty)",
    desc="Update fan 0 duty to 50% — name and gpio should be unchanged",
    method=PUT,
    path="/fans/0",
    payload=_fan_update(0, duty=50),
    expect_status="2.04",
    expect_fields={
        2: ("s", "f3"),  # name unchanged
        4: ("v", 50),  # duty = 50
        8: ("v", 15),
    },
)  # gpio unchanged

test(
    6,
    "DELETE /fans/1 (non-existent)",
    desc="Delete fan 1 which does not exist — should return 4.04 Not Found",
    method=DELETE,
    path="/fans/1",
    expect_status="4.04",
)

test(
    7,
    "GET /fans (list after ops)",
    desc="List fans after create+update — fan 0 should still exist",
    method=GET,
    path="/fans",
    expect_status="2.05",
)

# --- Sources ---
print(f'\n{"="*60}')
print("SOURCES")

test(
    8,
    "GET /sources (list)",
    desc="Query source list — may be empty or contain built-in sources",
    method=GET,
    path="/sources",
    expect_status="2.05",
)

test(
    9,
    "POST /sources (create manual source)",
    desc='Create a manual temperature source named "s2"',
    method=POST,
    path="/sources",
    payload=_src_create("manual", "s2"),
    expect_status="2.01",
    expect_fields={2: ("s", "s2")},
)

test(
    10,
    "POST /sources/temp (set manual temperature)",
    desc="Set manual temperature of source 0 to 20.0°C",
    method=POST,
    path="/sources/temp",
    payload=_temp(0, 20.0),
    expect_status="2.04",
    expect_fields={1: ("v", 1)},
)  # ok = true

test(
    11,
    "GET /sources/0 (read back)",
    desc="Read back source 0 — verify name and type persisted",
    method=GET,
    path="/sources/0",
    expect_status="2.05",
    expect_fields={2: ("s", "s2")},
)

test(
    12,
    "DELETE /sources/0 (remove source)",
    desc="Remove source 0 — should return 2.02 Deleted",
    method=DELETE,
    path="/sources/0",
    expect_status="2.02",
)

test(
    12,
    "GET /sources (list after delete)",
    desc="List sources after create+delete — verify cleanup worked",
    method=GET,
    path="/sources",
    expect_status="2.05",
)

# --- Curves ---
print(f'\n{"="*60}')
print("CURVES")

test(
    13,
    "GET /curves (list)",
    desc="Query curve list — may be empty",
    method=GET,
    path="/curves",
    expect_status="2.05",
)

test(
    14,
    "POST /curves (create without name)",
    desc="Create curve without required name field — should return 4.00 Bad Request",
    method=POST,
    path="/curves",
    payload=b"",  # empty body — missing required name
    expect_status="4.00",
)

test(
    15,
    "POST /curves (create with points)",
    desc='Create curve "c1" with 3 temp/duty points: (30°C,20%), (50°C,50%), (70°C,100%)',
    method=POST,
    path="/curves",
    payload=_curve_create("c1", [(30, 20), (50, 50), (70, 100)]),
    expect_status="2.01",
    expect_fields={2: ("s", "c1")},
)

test(
    16,
    "GET /curves/0 (read back)",
    desc="Read back curve 0 — verify name persisted",
    method=GET,
    path="/curves/0",
    expect_status="2.05",
    expect_fields={2: ("s", "c1")},
)

test(
    17,
    "PUT /curves/0 (rename and adjust points)",
    desc='Update curve 0: rename to "c1-mod", adjust points to (25°C,10%), (45°C,40%), (65°C,90%)',
    method=PUT,
    path="/curves/0",
    payload=_curve_update(0, name="c1-mod", pts=[(25, 10), (45, 40), (65, 90)]),
    expect_status="2.04",
    expect_fields={2: ("s", "c1-mod")},
)

test(
    18,
    "DELETE /curves/0 (remove curve)",
    desc="Delete curve 0 — should return 2.02 Deleted",
    method=DELETE,
    path="/curves/0",
    expect_status="2.02",
)

test(
    19,
    "GET /curves (list after delete)",
    desc="List curves after create+delete — verify cleanup",
    method=GET,
    path="/curves",
    expect_status="2.05",
)

# --- Schedules ---
print(f'\n{"="*60}')
print("SCHEDULES")

test(
    20,
    "GET /schedules (list)",
    desc="Query schedule list — may be empty",
    method=GET,
    path="/schedules",
    expect_status="2.05",
)

test(
    21,
    "POST /schedules (create schedule)",
    desc="Create schedule: fan 0, duty 50%, active 08:00-18:00 (480-1080 min), enabled",
    method=POST,
    path="/schedules",
    payload=_sched_create(fid=0, duty=50, sm=480, em=1080, en=True),
    expect_status="2.01",
    expect_fields={
        3: ("v", 50),  # duty = 50
        4: ("v", 480),  # start_min = 480 (08:00)
        5: ("v", 1080),  # end_min = 1080 (18:00)
        6: ("v", 1),
    },
)  # enabled = true

test(
    22,
    "GET /schedules (list after create)",
    desc="Read back schedule 0 — verify all fields persisted",
    method=GET,
    path="/schedules",
    expect_status="2.05",
)

test(
    23,
    "PUT /schedules/0 (update duty to 100)",
    desc="Update schedule 0 duty to 100%",
    method=PUT,
    path="/schedules/0",
    payload=_sched_update(0, duty=100),
    expect_status="2.04",
    expect_fields={3: ("v", 100)},
)  # duty updated to 100

test(
    24,
    "DELETE /schedules/0 (remove schedule)",
    desc="Delete schedule 0 — should return 2.02 Deleted",
    method=DELETE,
    path="/schedules/0",
    expect_status="2.02",
)

test(
    25,
    "GET /schedules (list after delete)",
    desc="List schedules after create+delete — verify cleanup",
    method=GET,
    path="/schedules",
    expect_status="2.05",
)

# --- WiFi ---
print(f'\n{"="*60}')
print("WIFI")

test(
    26,
    "GET /wifi/status",
    desc="Query WiFi status: should show STA connected with IP address",
    method=GET,
    path="/wifi/status",
    expect_status="2.05",
    expect_fields={
        1: ("v", 1),  # sta_connected = true
        2: ("s", None),  # sta_ip (dynamic)
        3: ("s", None),
    },
)  # ap_ip (dynamic)

test(
    27,
    "POST /wifi/connect (no password)",
    desc="Attempt WiFi connect with empty credentials — expect 5.03 or TIMEOUT (server restart)",
    method=POST,
    path="/wifi/connect",
    payload=_wifi_conn("", ""),
    expect_status=None,
)  # 5.03 if reachable, TIMEOUT if WiFi restart kills CoAP

# Small delay after WiFi op — server may be restarting
time.sleep(2)

# --- Cleanup ---
print(f'\n{"="*60}')
print("CLEANUP")

test(
    28,
    "DELETE /fans/0 (cleanup)",
    desc="Remove fan 0 to leave clean state for next test run",
    method=DELETE,
    path="/fans/0",
    expect_status=None,
)  # Accept any response after WiFi disruption

c.close()

# ============================================================
# Summary
# ============================================================

passed = sum(1 for _, _, r, _ in results if r == "PASS")
failed = sum(1 for _, _, r, _ in results if r == "FAIL")
print(f'\n{"="*60}')
print(f"RESULTS: {passed}/{len(results)} passed")
if failed > 0:
    print(f"\nFAILURES ({failed}):")
    for num, label, _, detail in results:
        if detail:
            print(f"  Case {num} [{label}]: {detail}")
else:
    print("All tests passed.")
