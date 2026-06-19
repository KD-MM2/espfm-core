#!/usr/bin/env python3
"""CoAP-to-HTTP bridge — translates CoAP requests to v2 HTTP REST API.
Usage: python coap_bridge.py <device_ip>
Then:  python coap_client.py 127.0.0.1 fans list"""
import socket, json, sys, urllib.request

HTTP = f'http://{sys.argv[1] if len(sys.argv)>1 else "192.168.0.50"}:80/api/v1'
COAP_GET, COAP_POST, COAP_PUT, COAP_DELETE = 1, 2, 3, 4

def http_req(method, path, body=None):
    url = HTTP + path
    data = json.dumps(body).encode() if body else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header('Content-Type', 'application/json')
    try: r = urllib.request.urlopen(req, timeout=5); return 0x45, r.read()
    except urllib.request.HTTPError as e: return {400:0x80,404:0x84,500:0xA0,503:0xA3}.get(e.code,0xA0), e.read()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('0.0.0.0', 5683))
print(f'Bridge :5683 -> {HTTP}')

while True:
    data, addr = s.recvfrom(1500)
    if len(data) < 4: continue
    code, tkl = data[1], data[0] & 0x0F
    mid = (data[2] << 8) | data[3]
    token = data[4:4+tkl]

    # Parse URI-Path (option 11)
    path, pos = '', 4 + tkl
    while pos < len(data) and data[pos] != 0xFF:
        dl = data[pos]; pos += 1
        d = (dl >> 4) & 0xF; l = dl & 0xF
        if d == 13: d = data[pos] + 13; pos += 1
        elif d == 14: d = (data[pos] << 8) + data[pos+1] + 269; pos += 2
        if l == 13: l = data[pos] + 13; pos += 1
        elif l == 14: l = (data[pos] << 8) + data[pos+1] + 269; pos += 2
        if d == 11: path += '/' + data[pos:pos+l].decode()
        pos += l

    body = data[data.index(0xFF)+1:] if 0xFF in data else b''
    js = json.loads(body) if body and code != COAP_GET else None

    rc, rb = 0x84, b'{}'
    try:
        if path.startswith('/fans'):
            if code == COAP_GET: rc, rb = http_req('GET', path)
            elif code == COAP_POST: rc, rb = http_req('PUT', '/fans', js)
            elif code == COAP_PUT: rc, rb = http_req('PUT', path, js)
            elif code == COAP_DELETE: rc, rb = http_req('DELETE', path)
        elif path.startswith('/sources'):
            if '/temp' in path and code == COAP_POST: rc, rb = http_req('POST', path, js)
            elif code == COAP_GET: rc, rb = http_req('GET', '/sources')
            elif code == COAP_POST: rc, rb = http_req('PUT', '/sources', js)
            elif code == COAP_DELETE: rc, rb = http_req('DELETE', path)
        elif path.startswith('/curves'):
            if code == COAP_GET: rc, rb = http_req('GET', path)
            elif code == COAP_POST: rc, rb = http_req('PUT', '/curves', js)
            elif code == COAP_PUT: rc, rb = http_req('PUT', path, js)
            elif code == COAP_DELETE: rc, rb = http_req('DELETE', path)
        elif path.startswith('/schedules'):
            if code == COAP_GET: rc, rb = http_req('GET', '/schedules')
            elif code == COAP_POST: rc, rb = http_req('PUT', '/schedules', js)
            elif code == COAP_PUT: rc, rb = http_req('PUT', path, js)
            elif code == COAP_DELETE: rc, rb = http_req('DELETE', path)
        elif '/wifi/scan' in path: rc, rb = http_req('GET', '/wifi/scan')
        elif '/wifi/status' in path: rc, rb = http_req('GET', '/wifi/status')
        elif '/wifi/connect' in path: rc, rb = http_req('POST', '/wifi/connect', js)
        elif '/system/info' in path: rc, rb = http_req('GET', '/system/info')
    except Exception as e: rc, rb = 0xA0, str(e).encode()

    resp = bytes([0x60 | tkl, rc & 0xFF, (mid >> 8) & 0xFF, mid & 0xFF]) + token + b'\xff' + rb
    s.sendto(resp, addr)
    print(f'{path} -> {rc>>5}.{rc&0x1F:02d} ({len(rb)}B)')
