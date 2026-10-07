# Minimal Chrome DevTools Protocol client (stdlib only): cdp.py METHOD [JSON-PARAMS] [page]
import json,os,socket,struct,sys,urllib.request,base64
target = 'page' if len(sys.argv) > 3 else 'browser'
if target == 'page':
    tabs = json.load(urllib.request.urlopen('http://127.0.0.1:9222/json'))
    url = [t for t in tabs if t['type'] == 'page'][0]['webSocketDebuggerUrl']
else:
    url = json.load(urllib.request.urlopen('http://127.0.0.1:9222/json/version'))['webSocketDebuggerUrl']
path = url.split('9222', 1)[1]
s = socket.create_connection(('127.0.0.1', 9222))
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1:9222\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n'.encode())
hdr = b''
while b'\r\n\r\n' not in hdr: hdr += s.recv(1)
def send(obj):
    data = json.dumps(obj).encode(); mask = os.urandom(4)
    n = len(data); head = bytes([0x81])
    head += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n) if n < 65536 else bytes([0x80 | 127]) + struct.pack('>Q', n)
    s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
def recvexact(n):
    b = b''
    while len(b) < n: b += s.recv(n - len(b))
    return b
def recv():
    b0, b1 = recvexact(2); n = b1 & 0x7f
    if n == 126: n = struct.unpack('>H', recvexact(2))[0]
    elif n == 127: n = struct.unpack('>Q', recvexact(8))[0]
    return json.loads(recvexact(n))
send({'id': 1, 'method': sys.argv[1], 'params': json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}})
while True:
    m = recv()
    if m.get('id') == 1: print(json.dumps(m.get('result', m.get('error')))); break
