#!/usr/bin/env python3
"""Run all 22 CoAP endpoint tests."""
import sys
sys.path.insert(0, '.')
from tests.coap_client import ESPFM

HOST = sys.argv[1] if len(sys.argv) > 1 else '192.168.0.50'
e = ESPFM(HOST)

results = []
def test(label, fn, *a, **kw):
    try:
        fn(*a, **kw) if callable(fn) else None
        r = 'PASS'
    except Exception as ex:
        r = f'ERR: {ex}'
    print(f'  [{r}] {label}')
    results.append((label, r))

print('--- System ---')
test('GET /system/info', e.system, 'info')

print('\n--- Fans ---')
test('GET /fans', e.fans, 'list')
test('GET /fans/0', e.fans, 'get', 0)
test('POST /fans (f3)', e.fans, 'create', pwm=15, name='f3')
test('PUT /fans/0 (duty=50)', e.fans, 'update', 0, duty=50)
test('DELETE /fans/1', e.fans, 'delete', 1)
test('GET /fans (after ops)', e.fans, 'list')

print('\n--- Sources ---')
test('GET /sources', e.sources, 'list')
test('POST /sources (s2)', e.sources, 'create', type='manual', name='s2')
test('POST /sources/temp', e.sources, 'temp', 0, temp_c=20.0)
test('DELETE /sources/0', e.sources, 'delete', 0)
test('GET /sources (after)', e.sources, 'list')

print('\n--- Curves ---')
test('GET /curves', e.curves, 'list')
test('POST /curves', e.curves, 'create', name='c1', points=[(30,20),(50,50),(70,100)])
test('GET /curves/0', e.curves, 'get', 0)
test('PUT /curves/0', e.curves, 'update', 0, name='c1-mod', points=[(25,10),(45,40),(65,90)])
test('DELETE /curves/0', e.curves, 'delete', 0)
test('GET /curves (after)', e.curves, 'list')

print('\n--- Schedules ---')
test('GET /schedules', e.schedules, 'list')
test('POST /schedules', e.schedules, 'create', fan=0, duty=50, start=480, end=1080)
test('GET /schedules (after create)', e.schedules, 'list')
test('PUT /schedules/0', e.schedules, 'update', 0, duty=100)
test('DELETE /schedules/0', e.schedules, 'delete', 0)
test('GET /schedules (after delete)', e.schedules, 'list')

print('\n--- WiFi ---')
test('GET /wifi/status', e.wifi, 'status')
test('GET /wifi/scan', e.wifi, 'scan')

test('DELETE /fans/0 (cleanup)', e.fans, 'delete', 0)

e.close()
passed = sum(1 for _, r in results if r == 'PASS')
print(f'\n{"="*40}\nResults: {passed}/{len(results)} passed')
for name, r in results:
    if r != 'PASS': print(f'  FAIL: {name}')
