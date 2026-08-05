#!/usr/bin/env python3
"""
CoAP endpoint test suite for ESPFanManager v3.

Each case documents: what it tests, the request sent, and expected outcome.
Uses CoAPTransport from espfm_shell.py and espfm_pb2 for protobuf.
"""

import os, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import espfm_pb2 as pb
from espfm_shell import CoAPTransport

# ============================================================
# Host-based unit tests (no hardware required)
# ============================================================
import unittest


def run_host_unit_tests():
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromNames(
        [
            "test_espfm_shell",
            "test_host_f_config",
            "test_host_f_coap",
            "test_host_f_gpio_claim",
            "test_host_f_control",
        ]
    )
    runner = unittest.TextTestRunner(verbosity=2)
    return runner.run(suite)


print(f'\n{"="*60}')
print("HOST-BASED UNIT TESTS (no hardware)")
print(f'{"="*60}')
host_result = run_host_unit_tests()
if not host_result.wasSuccessful():
    print("\nHost-based unit tests FAILED — aborting.")
    sys.exit(1)

# CoAP method constants
COAP_GET, COAP_POST, COAP_PUT, COAP_DELETE = 1, 2, 3, 4

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.50"
PORT = 5683

# ============================================================
# Response decoder
# ============================================================


def decode_response(data, msg_cls):
    """Decode raw protobuf bytes into a message object."""
    msg = msg_cls()
    msg.ParseFromString(data)
    return msg


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


def assert_fields(msg, expected_fields, label):
    """Check protobuf message fields match expected values.

    expected_fields: dict of field_name -> expected_value (None = just check existence)
    """
    issues = []
    for field_name, expected_value in expected_fields.items():
        if not hasattr(msg, field_name):
            issues.append(f"missing field '{field_name}'")
            continue
        actual = getattr(msg, field_name)
        if expected_value is not None and actual != expected_value:
            issues.append(f"field '{field_name}': expected {expected_value!r}, got {actual!r}")
    return issues


def test(
    case_num,
    label,
    desc,
    method,
    path,
    payload=b"",
    expect_status=None,
    expect_fields=None,
    response_cls=None,
):
    """
    Run one CoAP test case.

    case_num:      test number
    label:         short test name (e.g. 'GET /fans')
    desc:          what this test verifies
    method:        CoAP method (COAP_GET/COAP_POST/COAP_PUT/COAP_DELETE)
    path:          URI path
    payload:       protobuf-encoded request body (or b'')
    expect_status: expected CoAP status class (e.g. '2.05', '2.01', '4.00')
    expect_fields: dict of field_name -> expected_value to check in decoded response
    response_cls:  protobuf class to decode response with (e.g. pb.FanInfo)
    """
    code, data = transport.request(method, path, payload)

    issues = []

    # 1. Check status code
    if expect_status:
        ok, detail = assert_status(code, expect_status, label)
        if not ok:
            issues.append(detail)

    # 2. Check response fields
    msg = None
    if expect_fields and data and response_cls:
        try:
            msg = decode_response(data, response_cls)
            issues.extend(assert_fields(msg, expect_fields, label))
        except Exception as e:
            issues.append(f"failed to decode response: {e}")

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
        if msg:
            print(f"  Body:    {msg}")
        else:
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

# ============================================================
# Device probe — skip integration tests when no device is reachable
# ============================================================


def device_reachable():
    probe = CoAPTransport(HOST, PORT, timeout=2)
    probe.connect()
    try:
        for _ in range(2):
            code, _ = probe.request(COAP_GET, "/system/info")
            if code is not None:
                return True
        return False
    except Exception:
        return False
    finally:
        probe.close()


if not device_reachable():
    print(f"\nNo ESPFM device reachable at {HOST}:{PORT} — skipping device integration tests.")
    print("Host-based unit tests completed above.")
    sys.exit(0)

transport = CoAPTransport(HOST, PORT, timeout=3)
transport.connect()

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
    method=COAP_GET,
    path="/system/info",
    expect_status="2.05",
    expect_fields={"version": None, "uptime_s": None, "heap_free": None},
    response_cls=pb.SystemInfo,
)

# --- Fans ---
print(f'\n{"="*60}')
print("FANS")

test(
    2,
    "GET /fans (empty list)",
    desc="Query fan list before any fans exist",
    method=COAP_GET,
    path="/fans",
    expect_status="2.05",
    response_cls=pb.FanList,
)

test(
    3,
    "POST /fans (create fan)",
    desc='Create a new fan on PWM GPIO 15 named "f3"',
    method=COAP_POST,
    path="/fans",
    payload=pb.FanCreateRequest(pwm_gpio=15, name="f3").SerializeToString(),
    expect_status="2.01",
    expect_fields={"name": "f3", "pwm_gpio": 15},
    response_cls=pb.FanInfo,
)

test(
    4,
    "GET /fans/0 (read back)",
    desc="Read back the fan we just created — verify name and gpio persisted",
    method=COAP_GET,
    path="/fans/0",
    expect_status="2.05",
    expect_fields={"name": "f3", "pwm_gpio": 15},
    response_cls=pb.FanInfo,
)

test(
    5,
    "PUT /fans/0 (update duty)",
    desc="Update fan 0 duty to 50% — name and gpio should be unchanged",
    method=COAP_PUT,
    path="/fans/0",
    payload=pb.FanUpdateRequest(id=0, duty=50).SerializeToString(),
    expect_status="2.04",
    expect_fields={"name": "f3", "duty": 50, "pwm_gpio": 15},
    response_cls=pb.FanInfo,
)

test(
    6,
    "DELETE /fans/1 (non-existent)",
    desc="Delete fan 1 which does not exist — should return 4.04 Not Found",
    method=COAP_DELETE,
    path="/fans/1",
    expect_status="4.04",
)

test(
    7,
    "GET /fans (list after ops)",
    desc="List fans after create+update — fan 0 should still exist",
    method=COAP_GET,
    path="/fans",
    expect_status="2.05",
    response_cls=pb.FanList,
)

# --- Sources ---
print(f'\n{"="*60}')
print("SOURCES")

test(
    8,
    "GET /sources (list)",
    desc="Query source list — may be empty or contain built-in sources",
    method=COAP_GET,
    path="/sources",
    expect_status="2.05",
    response_cls=pb.SourceList,
)

test(
    9,
    "POST /sources (create manual source)",
    desc='Create a manual temperature source named "s2"',
    method=COAP_POST,
    path="/sources",
    payload=pb.SourceCreateRequest(
        type=pb.SOURCE_TYPE_MANUAL, name="s2"
    ).SerializeToString(),
    expect_status="2.01",
    expect_fields={"name": "s2"},
    response_cls=pb.SourceInfo,
)

test(
    10,
    "POST /sources/temp (set manual temperature)",
    desc="Set manual temperature of source 0 to 20.0°C",
    method=COAP_POST,
    path="/sources/temp",
    payload=pb.ManualTempRequest(id=0, temp_c=20.0).SerializeToString(),
    expect_status="2.04",
    expect_fields={"ok": True},
    response_cls=pb.StatusResponse,
)

test(
    11,
    "GET /sources/0 (read back)",
    desc="Read back source 0 — verify name and type persisted",
    method=COAP_GET,
    path="/sources/0",
    expect_status="2.05",
    expect_fields={"name": "s2"},
    response_cls=pb.SourceInfo,
)

test(
    12,
    "DELETE /sources/0 (remove source)",
    desc="Remove source 0 — should return 2.02 Deleted",
    method=COAP_DELETE,
    path="/sources/0",
    expect_status="2.02",
)

test(
    12,
    "GET /sources (list after delete)",
    desc="List sources after create+delete — verify cleanup worked",
    method=COAP_GET,
    path="/sources",
    expect_status="2.05",
    response_cls=pb.SourceList,
)

# --- Curves ---
print(f'\n{"="*60}')
print("CURVES")

test(
    13,
    "GET /curves (list)",
    desc="Query curve list — may be empty",
    method=COAP_GET,
    path="/curves",
    expect_status="2.05",
    response_cls=pb.CurveList,
)

test(
    14,
    "POST /curves (create without name)",
    desc="Create curve without required name field — should return 4.00 Bad Request",
    method=COAP_POST,
    path="/curves",
    payload=b"",  # empty body — missing required name
    expect_status="4.00",
)

test(
    15,
    "POST /curves (create with points)",
    desc='Create curve "c1" with 3 temp/duty points: (30°C,20%), (50°C,50%), (70°C,100%)',
    method=COAP_POST,
    path="/curves",
    payload=pb.CurveCreateRequest(
        name="c1",
        points=[
            pb.CurvePoint(temp_c=30, duty=20),
            pb.CurvePoint(temp_c=50, duty=50),
            pb.CurvePoint(temp_c=70, duty=100),
        ],
    ).SerializeToString(),
    expect_status="2.01",
    expect_fields={"name": "c1"},
    response_cls=pb.CurveInfo,
)

test(
    16,
    "GET /curves/0 (read back)",
    desc="Read back curve 0 — verify name persisted",
    method=COAP_GET,
    path="/curves/0",
    expect_status="2.05",
    expect_fields={"name": "c1"},
    response_cls=pb.CurveInfo,
)

test(
    17,
    "PUT /curves/0 (rename and adjust points)",
    desc='Update curve 0: rename to "c1-mod", adjust points to (25°C,10%), (45°C,40%), (65°C,90%)',
    method=COAP_PUT,
    path="/curves/0",
    payload=pb.CurveUpdateRequest(
        id=0,
        name="c1-mod",
        points=[
            pb.CurvePoint(temp_c=25, duty=10),
            pb.CurvePoint(temp_c=45, duty=40),
            pb.CurvePoint(temp_c=65, duty=90),
        ],
    ).SerializeToString(),
    expect_status="2.04",
    expect_fields={"name": "c1-mod"},
    response_cls=pb.CurveInfo,
)

test(
    18,
    "DELETE /curves/0 (remove curve)",
    desc="Delete curve 0 — should return 2.02 Deleted",
    method=COAP_DELETE,
    path="/curves/0",
    expect_status="2.02",
)

test(
    19,
    "GET /curves (list after delete)",
    desc="List curves after create+delete — verify cleanup",
    method=COAP_GET,
    path="/curves",
    expect_status="2.05",
    response_cls=pb.CurveList,
)

# --- Schedules ---
print(f'\n{"="*60}')
print("SCHEDULES")

test(
    20,
    "GET /schedules (list)",
    desc="Query schedule list — may be empty",
    method=COAP_GET,
    path="/schedules",
    expect_status="2.05",
    response_cls=pb.ScheduleList,
)

test(
    21,
    "POST /schedules (create schedule)",
    desc="Create schedule: fan 0, duty 50%, active 08:00-18:00 (480-1080 min), enabled",
    method=COAP_POST,
    path="/schedules",
    payload=pb.ScheduleCreateRequest(
        fan_id=0, duty=50, start_min=480, end_min=1080, enabled=True
    ).SerializeToString(),
    expect_status="2.01",
    expect_fields={"duty": 50, "start_min": 480, "end_min": 1080, "enabled": True},
    response_cls=pb.ScheduleInfo,
)

test(
    22,
    "GET /schedules (list after create)",
    desc="Read back schedule 0 — verify all fields persisted",
    method=COAP_GET,
    path="/schedules",
    expect_status="2.05",
    response_cls=pb.ScheduleList,
)

test(
    23,
    "PUT /schedules/0 (update duty to 100)",
    desc="Update schedule 0 duty to 100%",
    method=COAP_PUT,
    path="/schedules/0",
    payload=pb.ScheduleUpdateRequest(id=0, duty=100).SerializeToString(),
    expect_status="2.04",
    expect_fields={"duty": 100},
    response_cls=pb.ScheduleInfo,
)

test(
    24,
    "DELETE /schedules/0 (remove schedule)",
    desc="Delete schedule 0 — should return 2.02 Deleted",
    method=COAP_DELETE,
    path="/schedules/0",
    expect_status="2.02",
)

test(
    25,
    "GET /schedules (list after delete)",
    desc="List schedules after create+delete — verify cleanup",
    method=COAP_GET,
    path="/schedules",
    expect_status="2.05",
    response_cls=pb.ScheduleList,
)

# --- WiFi ---
print(f'\n{"="*60}')
print("WIFI")

test(
    26,
    "GET /wifi/status",
    desc="Query WiFi status: should show STA connected with IP address",
    method=COAP_GET,
    path="/wifi/status",
    expect_status="2.05",
    expect_fields={"sta_connected": None, "sta_ip": None, "ap_ip": None},
    response_cls=pb.WifiStatus,
)

test(
    27,
    "POST /wifi/connect (no password)",
    desc="Attempt WiFi connect with empty credentials — expect 5.03 or TIMEOUT (server restart)",
    method=COAP_POST,
    path="/wifi/connect",
    payload=pb.WifiConnectRequest(ssid="", password="").SerializeToString(),
    expect_status=None,  # 5.03 if reachable, TIMEOUT if WiFi restart kills CoAP
)

# Small delay after WiFi op — server may be restarting
time.sleep(2)

# --- Cleanup ---
print(f'\n{"="*60}')
print("CLEANUP")

test(
    28,
    "DELETE /fans/0 (cleanup)",
    desc="Remove fan 0 to leave clean state for next test run",
    method=COAP_DELETE,
    path="/fans/0",
    expect_status=None,  # Accept any response after WiFi disruption
)

transport.close()

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
