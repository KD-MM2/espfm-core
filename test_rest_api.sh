#!/bin/sh
# test_rest_api.sh — curl-based integration tests for ESPFanManager REST API
# Usage: ./test_rest_api.sh [device_ip]
# Default: 192.168.0.50

BASE="${1:-http://192.168.0.50}"
PASS=0
FAIL=0

assert_status() {
    local test_name="$1"
    local expected="$2"
    local actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  PASS: $test_name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $test_name (expected HTTP $expected, got $actual)"
        FAIL=$((FAIL + 1))
        echo "       Response: $4"
    fi
}

assert_json_field() {
    local test_name="$1"
    local json="$2"
    local field="$3"
    local expected="$4"
    local actual
    actual=$(echo "$json" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('$field',''))" 2>/dev/null)
    if [ "$expected" = "$actual" ]; then
        echo "  PASS: $test_name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $test_name (expected $field='$expected', got '$actual')"
        FAIL=$((FAIL + 1))
    fi
}

echo "========================================="
echo " ESPFanManager REST API Integration Test"
echo " Target: $BASE"
echo "========================================="
echo ""

# --- System Info ---
echo "[1] GET /api/v1/system/info"
RESP=$(curl -s -w "\n%{http_code}" "$BASE/api/v1/system/info")
HTTP=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
assert_status "System info returns 200" "200" "$HTTP" "$BODY"
STATUS=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin).get('status',''))" 2>/dev/null)
assert_json_field "System info status=ok" "$BODY" "" "ok" # hack: check status field
[ "$STATUS" = "ok" ] && echo "  PASS: status=ok" && PASS=$((PASS+1)) || { echo "  FAIL: status=$STATUS"; FAIL=$((FAIL+1)); }

# --- Fans ---
echo ""
echo "[2] GET /api/v1/fans (empty)"
RESP=$(curl -s -w "\n%{http_code}" "$BASE/api/v1/fans")
HTTP=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
assert_status "Fan list returns 200" "200" "$HTTP" "$BODY"

echo ""
echo "[3] PUT /api/v1/fans (create)"
RESP=$(curl -s -w "\n%{http_code}" -X PUT "$BASE/api/v1/fans" \
    -H "Content-Type: application/json" \
    -d '{"name":"TestFan1","pwm_gpio":5,"tach_gpio":255}')
HTTP=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
assert_status "Create fan returns 201" "201" "$HTTP" "$BODY"
FAN_ID=$(echo "$BODY" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('id',''))" 2>/dev/null)
echo "       Fan ID: $FAN_ID"

if [ -n "$FAN_ID" ]; then
    echo ""
    echo "[4] GET /api/v1/fans/$FAN_ID"
    RESP=$(curl -s -w "\n%{http_code}" "$BASE/api/v1/fans/$FAN_ID")
    HTTP=$(echo "$RESP" | tail -1)
    BODY=$(echo "$RESP" | sed '$d')
    assert_status "Get fan returns 200" "200" "$HTTP" "$BODY"

    echo ""
    echo "[5] PUT /api/v1/fans/$FAN_ID (update duty)"
    RESP=$(curl -s -w "\n%{http_code}" -X PUT "$BASE/api/v1/fans/$FAN_ID" \
        -H "Content-Type: application/json" \
        -d '{"mode":0,"duty":60}')
    HTTP=$(echo "$RESP" | tail -1)
    BODY=$(echo "$RESP" | sed '$d')
    assert_status "Update fan returns 200" "200" "$HTTP" "$BODY"

    echo ""
    echo "[6] GET /api/v1/fans (after create)"
    RESP=$(curl -s -w "\n%{http_code}" "$BASE/api/v1/fans")
    HTTP=$(echo "$RESP" | tail -1)
    BODY=$(echo "$RESP" | sed '$d')
    assert_status "Fan list with items returns 200" "200" "$HTTP" "$BODY"

    echo ""
    echo "[7] DELETE /api/v1/fans/$FAN_ID"
    RESP=$(curl -s -w "\n%{http_code}" -X DELETE "$BASE/api/v1/fans/$FAN_ID")
    HTTP=$(echo "$RESP" | tail -1)
    BODY=$(echo "$RESP" | sed '$d')
    assert_status "Delete fan returns 200" "200" "$HTTP" "$BODY"

    echo ""
    echo "[8] GET /api/v1/fans/$FAN_ID (after delete)"
    RESP=$(curl -s -w "\n%{http_code}" "$BASE/api/v1/fans/$FAN_ID")
    HTTP=$(echo "$RESP" | tail -1)
    assert_status "Get deleted fan returns 404" "404" "$HTTP" "$(echo "$RESP" | sed '$d')"
fi

# --- Sources ---
echo ""
echo "[9] PUT /api/v1/sources (create manual)"
RESP=$(curl -s -w "\n%{http_code}" -X PUT "$BASE/api/v1/sources" \
    -H "Content-Type: application/json" \
    -d '{"type":"manual","name":"ExtTemp"}')
HTTP=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
assert_status "Create source returns 201" "201" "$HTTP" "$BODY"
SRC_ID=$(echo "$BODY" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('id',''))" 2>/dev/null)

if [ -n "$SRC_ID" ]; then
    echo ""
    echo "[10] POST /api/v1/sources/temp"
    RESP=$(curl -s -w "\n%{http_code}" -X POST "$BASE/api/v1/sources/temp" \
        -H "Content-Type: application/json" \
        -d "{\"id\":$SRC_ID,\"temp_c\":28.5}")
    HTTP=$(echo "$RESP" | tail -1)
    assert_status "Post temp returns 200" "200" "$HTTP" "$(echo "$RESP" | sed '$d')"
fi

echo ""
echo "[11] GET /api/v1/sources"
RESP=$(curl -s -w "\n%{http_code}" "$BASE/api/v1/sources")
HTTP=$(echo "$RESP" | tail -1)
assert_status "Source list returns 200" "200" "$HTTP" "$(echo "$RESP" | sed '$d')"

# --- Curves ---
echo ""
echo "[12] PUT /api/v1/curves (create)"
RESP=$(curl -s -w "\n%{http_code}" -X PUT "$BASE/api/v1/curves" \
    -H "Content-Type: application/json" \
    -d '{"name":"Quiet","points":[{"temp_c":20,"duty":30},{"temp_c":40,"duty":60},{"temp_c":60,"duty":100}]}')
HTTP=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
assert_status "Create curve returns 201" "201" "$HTTP" "$BODY"
CURVE_ID=$(echo "$BODY" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('id',''))" 2>/dev/null)

if [ -n "$CURVE_ID" ]; then
    echo ""
    echo "[13] GET /api/v1/curves/$CURVE_ID"
    RESP=$(curl -s -w "\n%{http_code}" "$BASE/api/v1/curves/$CURVE_ID")
    HTTP=$(echo "$RESP" | tail -1)
    assert_status "Get curve returns 200" "200" "$HTTP" "$(echo "$RESP" | sed '$d')"

    echo ""
    echo "[14] PUT /api/v1/curves/$CURVE_ID (update)"
    RESP=$(curl -s -w "\n%{http_code}" -X PUT "$BASE/api/v1/curves/$CURVE_ID" \
        -H "Content-Type: application/json" \
        -d '{"name":"Silent","points":[{"temp_c":25,"duty":40},{"temp_c":50,"duty":90}]}')
    HTTP=$(echo "$RESP" | tail -1)
    assert_status "Update curve returns 200" "200" "$HTTP" "$(echo "$RESP" | sed '$d')"

    echo ""
    echo "[15] DELETE /api/v1/curves/$CURVE_ID"
    RESP=$(curl -s -w "\n%{http_code}" -X DELETE "$BASE/api/v1/curves/$CURVE_ID")
    HTTP=$(echo "$RESP" | tail -1)
    assert_status "Delete curve returns 200" "200" "$HTTP" "$(echo "$RESP" | sed '$d')"
fi

echo ""
echo "[16] GET /api/v1/curves"
RESP=$(curl -s -w "\n%{http_code}" "$BASE/api/v1/curves")
HTTP=$(echo "$RESP" | tail -1)
assert_status "Curve list returns 200" "200" "$HTTP" "$(echo "$RESP" | sed '$d')"

# --- Schedules ---
echo ""
echo "[17] PUT /api/v1/schedules (create)"
RESP=$(curl -s -w "\n%{http_code}" -X PUT "$BASE/api/v1/schedules" \
    -H "Content-Type: application/json" \
    -d '{"fan_id":0,"duty":80,"start_min":480,"end_min":1080,"enabled":true}')
HTTP=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
assert_status "Create schedule returns 201" "201" "$HTTP" "$BODY"
SCHED_ID=$(echo "$BODY" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('id',''))" 2>/dev/null)

if [ -n "$SCHED_ID" ]; then
    echo ""
    echo "[18] GET /api/v1/schedules"
    RESP=$(curl -s -w "\n%{http_code}" "$BASE/api/v1/schedules")
    HTTP=$(echo "$RESP" | tail -1)
    assert_status "Schedule list returns 200" "200" "$HTTP" "$(echo "$RESP" | sed '$d')"

    echo ""
    echo "[19] DELETE /api/v1/schedules/$SCHED_ID"
    RESP=$(curl -s -w "\n%{http_code}" -X DELETE "$BASE/api/v1/schedules/$SCHED_ID")
    HTTP=$(echo "$RESP" | tail -1)
    assert_status "Delete schedule returns 200" "200" "$HTTP" "$(echo "$RESP" | sed '$d')"
fi

# --- CORS ---
echo ""
echo "[20] OPTIONS /api/v1/fans (CORS preflight)"
RESP=$(curl -s -w "\n%{http_code}" -X OPTIONS "$BASE/api/v1/fans" \
    -H "Origin: http://example.com" \
    -H "Access-Control-Request-Method: GET")
HTTP=$(echo "$RESP" | tail -1)
assert_status "OPTIONS returns 204" "204" "$HTTP" "$(echo "$RESP" | sed '$d')"

# --- Error cases ---
echo ""
echo "[21] GET /api/v1/fans/99 (not found)"
RESP=$(curl -s -w "\n%{http_code}" "$BASE/api/v1/fans/99")
HTTP=$(echo "$RESP" | tail -1)
assert_status "Non-existent fan returns 404" "404" "$HTTP" "$(echo "$RESP" | sed '$d')"

echo ""
echo "[22] PUT /api/v1/fans (missing name)"
RESP=$(curl -s -w "\n%{http_code}" -X PUT "$BASE/api/v1/fans" \
    -H "Content-Type: application/json" \
    -d '{"pwm_gpio":5}')
HTTP=$(echo "$RESP" | tail -1)
assert_status "Missing name returns 400" "400" "$HTTP" "$(echo "$RESP" | sed '$d')"

echo ""
echo "[23] PUT /api/v1/curves (missing points)"
RESP=$(curl -s -w "\n%{http_code}" -X PUT "$BASE/api/v1/curves" \
    -H "Content-Type: application/json" \
    -d '{"name":"Bad"}')
HTTP=$(echo "$RESP" | tail -1)
assert_status "Missing points returns 400" "400" "$HTTP" "$(echo "$RESP" | sed '$d')"

echo ""
echo "========================================="
echo " RESULTS: $PASS passed, $FAIL failed"
echo "========================================="
