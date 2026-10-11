# mediawatch.py PORT SECONDS [reload]: Chrome DevTools (stdlib only) on the first page:
# prints the media player decoder properties/errors, then video playback quality.
import json, os, socket, struct, sys, time, urllib.request, base64
port, secs = int(sys.argv[1]), float(sys.argv[2])
tabs = json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json'))
url = [t for t in tabs if t['type'] == 'page'][0]['webSocketDebuggerUrl']
path = url.split(str(port), 1)[1]
s = socket.create_connection(('127.0.0.1', port))
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n'.encode())
hdr = b''
while b'\r\n\r\n' not in hdr: hdr += s.recv(1)
def send(i, method, params={}):
    data = json.dumps({'id': i, 'method': method, 'params': params}).encode(); mask = os.urandom(4)
    n = len(data); head = bytes([0x81])
    head += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n) if n < 65536 else bytes([0x80 | 127]) + struct.pack('>Q', n)
    s.sendall(head + mask + bytes(b ^ mask[k % 4] for k, b in enumerate(data)))
def recvexact(n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise EOFError
        b += c
    return b
def recv():
    b0, b1 = recvexact(2); n = b1 & 0x7f
    if n == 126: n = struct.unpack('>H', recvexact(2))[0]
    elif n == 127: n = struct.unpack('>Q', recvexact(8))[0]
    return json.loads(recvexact(n))
send(1, 'Media.enable')
if len(sys.argv) > 3 and sys.argv[3] == 'reload': send(2, 'Page.reload')
end = time.time() + secs
s.settimeout(1.0)
interesting = ('decoder', 'Decoder', 'error', 'Error', 'kVideo', 'resolution', 'kIsPlatform')
while time.time() < end:
    try: m = recv()
    except socket.timeout: continue
    meth = m.get('method', '')
    if meth == 'Media.playerPropertiesChanged':
        for p in m['params']['properties']:
            if any(k in p['name'] for k in interesting): print('prop', p['name'], '=', p['value'], flush=True)
    elif meth == 'Media.playerErrorsRaised':
        print('error', json.dumps(m['params'])[:400], flush=True)
    elif meth == 'Media.playerMessagesLogged':
        for msg in m['params']['messages']:
            if msg['level'] in ('error', 'warning') or 'ecoder' in msg['message']:
                print('msg', msg['level'], msg['message'][:300], flush=True)
expr = "(()=>{const v=document.querySelector('video');const q=v.getVideoPlaybackQuality();return JSON.stringify({t:v.currentTime,paused:v.paused,w:v.videoWidth,h:v.videoHeight,decoded:q.totalVideoFrames,dropped:q.droppedVideoFrames})})()"
send(3, 'Runtime.evaluate', {'expression': expr, 'returnByValue': True})
while True:
    try: m = recv()
    except socket.timeout: break
    if m.get('id') == 3: print('quality', m['result']['result'].get('value')); break
