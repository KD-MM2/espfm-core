#!/usr/bin/env python3
"""test_rest_api.py — Integration tests for ESPFanManager REST API.
Usage: python test_rest_api.py [device_ip]
Default target: http://192.168.0.50
"""

import json, sys, urllib.request, urllib.error

BASE = f"http://{sys.argv[1]}" if len(sys.argv) > 1 else "http://192.168.0.50"
PASS = 0
FAIL = 0

def request(method, path, body=None):
    url = f"{BASE}{path}"
    data = json.dumps(body).encode() if body else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        body_text = e.read().decode()
        try:
            return e.code, json.loads(body_text)
        except:
            return e.code, {"status": "error", "error": {"message": body_text[:200]}}
    except Exception as e:
        return 0, {"status": "error", "error": {"message": str(e)}}

def check(test_name, condition, detail=""):
    global PASS, FAIL
    if condition:
        print(f"  PASS: {test_name}")
        PASS += 1
    else:
        print(f"  FAIL: {test_name} {detail}")
        FAIL += 1

print("=" * 50)
print(" ESPFanManager REST API Integration Test")
print(f" Target: {BASE}")
print("=" * 50)

# 1. System info
print("\n[1] GET /api/v1/system/info")
code, data = request("GET", "/api/v1/system/info")
check("returns 200", code == 200, f"got {code}")
check("status=ok", data.get("status") == "ok", f"got {data.get('status')}")
info = data.get("data", {})
check("has version", "version" in info)
check("has uptime_s", "uptime_s" in info)
check("has heap_free", "heap_free" in info)
print(f"    version={info.get('version')} uptime={info.get('uptime_s')}s heap={info.get('heap_free')}")

# 2. List fans (empty)
print("\n[2] GET /api/v1/fans (empty)")
code, data = request("GET", "/api/v1/fans")
check("returns 200", code == 200, f"got {code}")

# 3. Create fan
print("\n[3] PUT /api/v1/fans (create)")
code, data = request("PUT", "/api/v1/fans", {"name": "TestFan1", "pwm_gpio": 5, "tach_gpio": 255})
check("returns 201", code == 201, f"got {code}: {data}")
fan_id = data.get("data", {}).get("id")
check("has id", fan_id is not None)

if fan_id is not None:
    # 4. Get single fan
    print(f"\n[4] GET /api/v1/fans/{fan_id}")
    code, data = request("GET", f"/api/v1/fans/{fan_id}")
    check("returns 200", code == 200, f"got {code}")
    fan = data.get("data", {})
    check("name matches", fan.get("name") == "TestFan1", f"got {fan.get('name')}")

    # 5. Update fan duty
    print(f"\n[5] PUT /api/v1/fans/{fan_id} (update)")
    code, data = request("PUT", f"/api/v1/fans/{fan_id}", {"mode": 0, "duty": 60})
    check("returns 200", code == 200, f"got {code}")
    check("duty=60", data.get("data", {}).get("duty") == 60)

    # 6. List fans (with item)
    print("\n[6] GET /api/v1/fans (after create)")
    code, data = request("GET", "/api/v1/fans")
    check("returns 200", code == 200)
    check("has fans", len(data.get("data", [])) > 0)

    # 7. Delete fan
    print(f"\n[7] DELETE /api/v1/fans/{fan_id}")
    code, data = request("DELETE", f"/api/v1/fans/{fan_id}")
    check("returns 200", code == 200, f"got {code}")

    # 8. Get deleted fan -> 404
    print(f"\n[8] GET /api/v1/fans/{fan_id} (not found)")
    code, data = request("GET", f"/api/v1/fans/{fan_id}")
    check("returns 404", code == 404, f"got {code}")

# 9. Create source
print("\n[9] PUT /api/v1/sources (create manual)")
code, data = request("PUT", "/api/v1/sources", {"type": "manual", "name": "ExtTemp"})
check("returns 201", code == 201, f"got {code}: {data}")
src_id = data.get("data", {}).get("id")

if src_id is not None:
    # 10. Post temp
    print("\n[10] POST /api/v1/sources/temp")
    code, data = request("POST", "/api/v1/sources/temp", {"id": src_id, "temp_c": 28.5})
    check("returns 200", code == 200, f"got {code}: {data}")

# 11. List sources
print("\n[11] GET /api/v1/sources")
code, data = request("GET", "/api/v1/sources")
check("returns 200", code == 200, f"got {code}")

# 12. Create curve
print("\n[12] PUT /api/v1/curves (create)")
code, data = request("PUT", "/api/v1/curves", {
    "name": "Quiet",
    "points": [{"temp_c": 20, "duty": 30}, {"temp_c": 40, "duty": 60}, {"temp_c": 60, "duty": 100}]
})
check("returns 201", code == 201, f"got {code}: {data}")
curve_id = data.get("data", {}).get("id")

if curve_id is not None:
    # 13. Get curve
    print(f"\n[13] GET /api/v1/curves/{curve_id}")
    code, data = request("GET", f"/api/v1/curves/{curve_id}")
    check("returns 200", code == 200, f"got {code}")
    check("3 points", len(data.get("data", {}).get("points", [])) == 3)

    # 14. Update curve
    print(f"\n[14] PUT /api/v1/curves/{curve_id} (update)")
    code, data = request("PUT", f"/api/v1/curves/{curve_id}", {
        "name": "Silent",
        "points": [{"temp_c": 25, "duty": 40}, {"temp_c": 50, "duty": 90}]
    })
    check("returns 200", code == 200, f"got {code}")

    # 15. Delete curve
    print(f"\n[15] DELETE /api/v1/curves/{curve_id}")
    code, data = request("DELETE", f"/api/v1/curves/{curve_id}")
    check("returns 200", code == 200, f"got {code}")

# 16. List curves
print("\n[16] GET /api/v1/curves")
code, data = request("GET", "/api/v1/curves")
check("returns 200", code == 200, f"got {code}")

# 17. Create schedule
print("\n[17] PUT /api/v1/schedules (create)")
code, data = request("PUT", "/api/v1/schedules", {
    "fan_id": 0, "duty": 80, "start_min": 480, "end_min": 1080, "enabled": True
})
check("returns 201", code == 201, f"got {code}: {data}")
sched_id = data.get("data", {}).get("id")

if sched_id is not None:
    # 18. List schedules
    print("\n[18] GET /api/v1/schedules")
    code, data = request("GET", "/api/v1/schedules")
    check("returns 200", code == 200, f"got {code}")
    check("has schedules", len(data.get("data", [])) > 0, f"got {data.get('data')}")

    # 19. Delete schedule
    print(f"\n[19] DELETE /api/v1/schedules/{sched_id}")
    code, data = request("DELETE", f"/api/v1/schedules/{sched_id}")
    check("returns 200", code == 200, f"got {code}")

# 20. CORS preflight
print("\n[20] OPTIONS /api/v1/fans (CORS)")
req = urllib.request.Request(f"{BASE}/api/v1/fans", method="OPTIONS")
req.add_header("Origin", "http://example.com")
req.add_header("Access-Control-Request-Method", "GET")
try:
    with urllib.request.urlopen(req, timeout=5) as resp:
        check("returns 204", resp.status == 204, f"got {resp.status}")
        cors = resp.getheader("Access-Control-Allow-Origin", "")
        check("CORS header present", cors == "*", f"got '{cors}'")
except Exception as e:
    check("returns 204", False, str(e))

# 21. Error: non-existent fan
print("\n[21] GET /api/v1/fans/99 (not found)")
code, data = request("GET", "/api/v1/fans/99")
check("returns 404", code == 404, f"got {code}")

# 22. Error: missing name
print("\n[22] PUT /api/v1/fans (missing name)")
code, data = request("PUT", "/api/v1/fans", {"pwm_gpio": 5})
check("returns 400", code == 400, f"got {code}: {data}")

# 23. Error: missing points
print("\n[23] PUT /api/v1/curves (missing points)")
code, data = request("PUT", "/api/v1/curves", {"name": "Bad"})
check("returns 400", code == 400, f"got {code}: {data}")

print("\n" + "=" * 50)
print(f" RESULTS: {PASS} passed, {FAIL} failed, {PASS+FAIL} total")
if FAIL == 0:
    print(" ALL TESTS PASSED!")
print("=" * 50)
