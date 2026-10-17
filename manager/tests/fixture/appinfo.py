# Writes a minimal Steam appinfo.vdf (format 29: key names in a string table) for the manager's self-test.
# Usage: appinfo.py <out> <appid>:<onlyvr 0|1> ...
import struct, sys
strings = []
def idx(s):
    if s not in strings: strings.append(s)
    return struct.pack('<I', strings.index(s))
def node(k): return b'\x00' + idx(k)
def sval(k, v): return b'\x01' + idx(k) + v.encode() + b'\x00'
def ival(k, v): return b'\x02' + idx(k) + struct.pack('<i', v)
END = b'\x08'
body = b''
for a in sys.argv[2:]:
    appid, vr = a.split(':')
    kv = node('appinfo') + ival('appid', int(appid)) + node('common') + sval('name', 'App ' + appid)
    if vr == '1': kv += sval('openvrsupport', '1') + sval('onlyvrsupport', '1')
    kv += sval('type', 'Game') + END + node('extended') + sval('developer', 'x') + END + END + END
    head = struct.pack('<IIQ', 2, 1700000000, 0) + b'\0' * 20 + struct.pack('<I', 1) + b'\0' * 20
    entry = head + kv
    body += struct.pack('<II', int(appid), len(entry)) + entry
body += struct.pack('<I', 0)
table = struct.pack('<I', len(strings)) + b''.join(s.encode() + b'\x00' for s in strings)
off = 16 + len(body)
open(sys.argv[1], 'wb').write(struct.pack('<IIQ', 0x07564429, 1, off) + body + table)
