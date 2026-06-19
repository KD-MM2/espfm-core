#!/usr/bin/env python3
"""
CoAP + Protobuf test client for ESPFanManager v3.

Usage:
    python coap_client.py <host> [command]

    # CRUD examples:
    python coap_client.py 192.168.4.1 fans list
    python coap_client.py 192.168.4.1 fans get 0
    python coap_client.py 192.168.4.1 fans create --pwm 13 --name f1
    python coap_client.py 192.168.4.1 fans update 0 --duty 80 --mode 1
    python coap_client.py 192.168.4.1 fans delete 0

    python coap_client.py 192.168.4.1 sources list
    python coap_client.py 192.168.4.1 sources create --type manual --name s1
    python coap_client.py 192.168.4.1 sources delete 0
    python coap_client.py 192.168.4.1 sources temp 0 35.5

    python coap_client.py 192.168.4.1 curves list
    python coap_client.py 192.168.4.1 curves create --name c1 --points 30:20,50:50,70:100

    python coap_client.py 192.168.4.1 schedules list
    python coap_client.py 192.168.4.1 schedules create --fan 0 --duty 50 --start 480 --end 1080
    python coap_client.py 192.168.4.1 schedules update 0 --enabled 1

    python coap_client.py 192.168.4.1 wifi scan
    python coap_client.py 192.168.4.1 wifi status
    python coap_client.py 192.168.4.1 wifi connect --ssid MyWiFi --pass secret

    python coap_client.py 192.168.4.1 system info
"""

import socket, struct, sys, json, argparse, time


# ============================================================
# Minimal Protobuf encoder/decoder
# ============================================================

def _v_encode(value):
    result = []
    while value > 0x7F:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value & 0x7F)
    return bytes(result)

def _v_decode(data, pos):
    value = shift = 0
    while pos < len(data):
        b = data[pos]; pos += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            return value, pos
    return value, pos

def _wire(tag, wt, val):
    return _v_encode((tag << 3) | wt) + val

def _u32(tag, v): return _wire(tag, 0, _v_encode(v))
def _i32(tag, v):
    val = v & 0xFFFFFFFF
    return _wire(tag, 0, _v_encode(val))
def _f32(tag, v): return _wire(tag, 5, struct.pack('<f', v))
def _bool(tag, v): return _wire(tag, 0, b'\x01' if v else b'\x00')
def _str(tag, v):
    b = v.encode() if isinstance(v, str) else v
    return _wire(tag, 2, _v_encode(len(b)) + b)
def _enum(tag, v): return _u32(tag, v)

def _decode(data):
    result, pos = {}, 0
    while pos < len(data):
        tw, pos = _v_decode(data, pos)
        fn, wt = tw >> 3, tw & 7
        if wt == 0:
            v, pos = _v_decode(data, pos); result[fn] = ('v', v)
        elif wt == 5 and pos + 4 <= len(data):
            result[fn] = ('f', struct.unpack('<f', data[pos:pos+4])[0]); pos += 4
        elif wt == 2:
            ln, pos = _v_decode(data, pos)
            sub = data[pos:pos+ln]; pos += ln
            try: s = sub.decode(); result[fn] = ('s', s) if all(ord(c)<128 for c in s) else ('b', sub)
            except: result[fn] = ('m', _decode(sub))
        else: break
    return result

def _get(d, f, defv=None):
    return d[f][1] if f in d else defv


# ============================================================
# Message builders  (field numbers match espfm.proto)
# ============================================================

def _fan_create(pwm, name, tach=0):
    m = _u32(1, pwm) + _str(3, name)
    if tach: m += _u32(2, tach)
    return m

def _fan_update(id, **kw):
    m = _u32(1, id)
    if 'mode' in kw: m += _enum(2, kw['mode'])
    if 'duty' in kw: m += _u32(3, kw['duty'])
    if 'source_id' in kw: m += _u32(4, kw['source_id'])
    if 'curve_id' in kw: m += _u32(5, kw['curve_id'])
    if 'schedule_id' in kw: m += _u32(6, kw['schedule_id'])
    if 'group_id' in kw: m += _u32(7, kw['group_id'])
    if 'inverted' in kw: m += _bool(8, kw['inverted'])
    return m

def _src_create(typ, name, gpio=0):
    tm = {'manual': 0, 'ntc': 1, 'ds18b20': 2}
    m = _enum(1, tm.get(typ, typ)) + _str(2, name)
    if gpio: m += _u32(3, gpio)
    return m

def _temp(id, tc):
    return _u32(1, id) + _f32(2, tc)

def _curve_create(name, pts):
    m = _str(1, name)
    for tc, d in pts:
        m += _wire(3, 2, _v_encode(len(_f32(1, tc) + _u32(2, d))) + _f32(1, tc) + _u32(2, d))
    return m

def _curve_update(id, name=None, pts=None):
    m = _u32(1, id)
    if name: m += _str(2, name)
    if pts:
        for tc, d in pts:
            m += _wire(3, 2, _v_encode(len(_f32(1, tc) + _u32(2, d))) + _f32(1, tc) + _u32(2, d))
    return m

def _sched_create(fid, duty, sm, em, en=True):
    m = _u32(1, fid) + _u32(2, duty) + _u32(3, sm) + _u32(4, em)
    if en is not None: m += _bool(5, en)
    return m

def _sched_update(id, **kw):
    m = _u32(1, id)
    if 'fan_id' in kw: m += _u32(2, kw['fan_id'])
    if 'duty' in kw: m += _u32(3, kw['duty'])
    if 'start_min' in kw: m += _u32(4, kw['start_min'])
    if 'end_min' in kw: m += _u32(5, kw['end_min'])
    if 'enabled' in kw: m += _bool(6, kw['enabled'])
    return m

def _wifi_conn(ssid, pw):
    return _str(1, ssid) + _str(2, pw)


# ============================================================
# CoAP client (raw UDP)
# ============================================================

GET, POST, PUT, DELETE = 1, 2, 3, 4

class CoAP:
    def __init__(self, host, port=5683, timeout=3):
        self.host, self.port = host, port
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(timeout)
        self._mid = int(time.time() * 1000) & 0xFFFF

    def _mid_next(self):
        self._mid = (self._mid + 1) & 0xFFFF
        return self._mid

    def req(self, method, path, payload=b''):
        segs = [s for s in path.split('/') if s]
        tok = struct.pack('>I', int(time.time() * 1e6) & 0xFFFFFFFF)[:4]
        hdr = bytes([0x40 | len(tok), method, (self._mid_next() >> 8) & 0xFF, self._mid & 0xFF]) + tok
        # URI-Path options (11)
        for i, s in enumerate(segs):
            d = 11 if i == 0 else 0
            hdr += bytes([(d << 4) | len(s)]) + s.encode()
        if payload:
            hdr += b'\xff' + payload
        self.s.sendto(hdr, (self.host, self.port))
        try:
            data, _ = self.s.recvfrom(4096)
            code = data[1]
            pos = 4 + (data[0] & 0x0F)
            while pos < len(data) and data[pos] != 0xFF:
                dl = data[pos]; pos += 1
                d = (dl >> 4) & 0xF; l = dl & 0xF
                if d == 13: pos += 1
                elif d == 14: pos += 2
                if l == 13: l = data[pos] + 13; pos += 1
                elif l == 14: l = (data[pos] << 8) + data[pos+1] + 269; pos += 2
                pos += l
            payload = data[pos+1:] if pos < len(data) and data[pos] == 0xFF else b''
            return code, payload
        except socket.timeout:
            return None, None

    def close(self):
        self.s.close()

CODES = {0x41:'201',0x42:'202',0x44:'204',0x45:'205',0x80:'400',0x84:'404',0xA3:'503'}


# ============================================================
# Command Router
# ============================================================

class ESPFM:
    def __init__(self, host, port=5683, timeout=3):
        self.c = CoAP(host, port, timeout)

    def _r(self, method, path, payload=b''):
        code, data = self.c.req(method, path, payload)
        print(f"  Status: {CODES.get(code, f'{code>>5}.{code&0x1F:02d}' if code else 'TIMEOUT')}")
        if data:
            try:
                d = _decode(data)
                print(json.dumps(d, indent=2, default=str))
            except:
                print(f"  Raw: {data.hex()}")

    # Fans
    def fans(self, act, *a, **kw):
        if act == 'list': self._r(GET, '/fans')
        elif act == 'get': self._r(GET, f'/fans/{a[0]}')
        elif act == 'create': self._r(POST, '/fans', _fan_create(kw['pwm'], kw['name'], kw.get('tach', 0)))
        elif act == 'update': self._r(PUT, f'/fans/{a[0]}', _fan_update(a[0], **kw))
        elif act == 'delete': self._r(DELETE, f'/fans/{a[0]}')

    # Sources
    def sources(self, act, *a, **kw):
        if act == 'list': self._r(GET, '/sources')
        elif act == 'create': self._r(POST, '/sources', _src_create(kw['type'], kw['name'], kw.get('gpio', 0)))
        elif act == 'delete': self._r(DELETE, f'/sources/{a[0]}')
        elif act == 'temp': self._r(POST, '/sources/temp', _temp(a[0], a[1]))

    # Curves
    def curves(self, act, *a, **kw):
        if act == 'list': self._r(GET, '/curves')
        elif act == 'get': self._r(GET, f'/curves/{a[0]}')
        elif act == 'create': self._r(POST, '/curves', _curve_create(kw['name'], kw['points']))
        elif act == 'update': self._r(PUT, f'/curves/{a[0]}', _curve_update(a[0], kw.get('name'), kw.get('points')))
        elif act == 'delete': self._r(DELETE, f'/curves/{a[0]}')

    # Schedules
    def schedules(self, act, *a, **kw):
        if act == 'list': self._r(GET, '/schedules')
        elif act == 'create': self._r(POST, '/schedules', _sched_create(kw['fan'], kw['duty'], kw['start'], kw['end'], kw.get('enabled', True)))
        elif act == 'update': self._r(PUT, f'/schedules/{a[0]}', _sched_update(a[0], **kw))
        elif act == 'delete': self._r(DELETE, f'/schedules/{a[0]}')

    # WiFi
    def wifi(self, act, **kw):
        if act == 'scan': self._r(GET, '/wifi/scan')
        elif act == 'status': self._r(GET, '/wifi/status')
        elif act == 'connect': self._r(POST, '/wifi/connect', _wifi_conn(kw['ssid'], kw.get('pass', '')))

    # System
    def system(self, act):
        if act == 'info': self._r(GET, '/system/info')

    def close(self):
        self.c.close()


# ============================================================
# CLI
# ============================================================

def _pts(s):
    return [(float(p.split(':')[0]), int(p.split(':')[1])) for p in s.split(',')]

def main():
    p = argparse.ArgumentParser(description='ESPFanManager v3 CoAP+PB Client')
    p.add_argument('host')
    p.add_argument('--port', type=int, default=5683)
    p.add_argument('--timeout', type=float, default=3)

    sp = p.add_subparsers(dest='cmd')

    f = sp.add_parser('fans'); f.add_argument('action', choices=['list','get','create','update','delete']); f.add_argument('id', nargs='?', type=int); f.add_argument('--pwm', type=int); f.add_argument('--tach', type=int, default=0); f.add_argument('--name'); f.add_argument('--mode', type=int, choices=[0,1]); f.add_argument('--duty', type=int); f.add_argument('--source', type=int, dest='source_id'); f.add_argument('--curve', type=int, dest='curve_id'); f.add_argument('--schedule', type=int, dest='schedule_id'); f.add_argument('--group', type=int, dest='group_id'); f.add_argument('--inverted', type=int, choices=[0,1])

    s = sp.add_parser('sources'); s.add_argument('action', choices=['list','create','delete','temp']); s.add_argument('id', nargs='?', type=int); s.add_argument('--type'); s.add_argument('--name'); s.add_argument('--gpio', type=int, default=0); s.add_argument('--temp', type=float, dest='temp_c')

    c = sp.add_parser('curves'); c.add_argument('action', choices=['list','get','create','update','delete']); c.add_argument('id', nargs='?', type=int); c.add_argument('--name'); c.add_argument('--points')

    sc = sp.add_parser('schedules'); sc.add_argument('action', choices=['list','create','update','delete']); sc.add_argument('id', nargs='?', type=int); sc.add_argument('--fan', type=int, dest='fan_id'); sc.add_argument('--duty', type=int); sc.add_argument('--start', type=int, dest='start_min'); sc.add_argument('--end', type=int, dest='end_min'); sc.add_argument('--enabled', type=int, choices=[0,1])

    w = sp.add_parser('wifi'); w.add_argument('action', choices=['scan','status','connect']); w.add_argument('--ssid'); w.add_argument('--pass', dest='password', default='')

    sy = sp.add_parser('system'); sy.add_argument('action', choices=['info'])

    args = p.parse_args()
    if not args.cmd: p.print_help(); return

    e = ESPFM(args.host, args.port, args.timeout)
    try:
        if args.cmd == 'fans':
            kw = {}
            for k in ['mode','duty','source_id','curve_id','schedule_id','group_id']:
                if getattr(args, k, None) is not None: kw[k] = getattr(args, k)
            if args.inverted is not None: kw['inverted'] = bool(args.inverted)
            if args.pwm is not None: kw['pwm'] = args.pwm
            if args.name: kw['name'] = args.name
            if args.tach: kw['tach'] = args.tach
            e.fans(args.action, args.id, **kw)
        elif args.cmd == 'sources':
            kw = {}
            for k in ['type','name','gpio']:
                if getattr(args, k, None) is not None: kw[k] = getattr(args, k)
            e.sources(args.action, args.id, args.temp_c, **kw)
        elif args.cmd == 'curves':
            kw = {}
            if args.name: kw['name'] = args.name
            if args.points: kw['points'] = _pts(args.points)
            e.curves(args.action, args.id, **kw)
        elif args.cmd == 'schedules':
            kw = {}
            for k in ['fan_id','duty','start_min','end_min']:
                if getattr(args, k, None) is not None: kw[k.replace('_id','').replace('start_','start').replace('end_','end')] = getattr(args, k)
            if args.enabled is not None: kw['enabled'] = bool(args.enabled)
            e.schedules(args.action, args.id, **kw)
        elif args.cmd == 'wifi':
            kw = {}
            if args.ssid: kw['ssid'] = args.ssid
            if args.password: kw['pass'] = args.password
            e.wifi(args.action, **kw)
        elif args.cmd == 'system':
            e.system(args.action)
    finally:
        e.close()

if __name__ == '__main__':
    main()
