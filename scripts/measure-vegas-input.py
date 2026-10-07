#!/usr/bin/env python3
"""Bounded native Ilitek -> wvkbd -> Foot timing; never records typed contents.

Leaves the user's normal terminal and keyboard running. The temporary terminal
reads raw bytes, records monotonic receipt timestamps and exits after the trial.
Wayland tracing is limited to that test terminal. Files live on the phone's /run.
"""
import argparse
import base64
import json
from pathlib import Path
import re
import statistics
import time
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))
URL = 'http://192.168.77.1:8080/cgi-bin/command'
CLIENT = '''import os,tty,termios,select,time,json
p="/run/user/1000/vegas-latency"
old=termios.tcgetattr(0)
tty.setraw(0)
try:
    with open(p+"/received.jsonl","w",buffering=1) as log:
        os.write(1,b"Input timing test; please do not type.\\r\\n")
        open(p+"/ready","w").close()
        deadline=time.monotonic()+20
        count=0
        while count<12 and time.monotonic()<deadline:
            if not select.select([0],[],[],0.2)[0]: continue
            data=os.read(0,1)
            if not data: break
            now=time.monotonic_ns()
            count+=1
            log.write(json.dumps({"n":count,"received_ns":now})+"\\n")
            os.write(1,("Input %02d\\r\\n"%count).encode())
finally:
    termios.tcsetattr(0,termios.TCSANOW,old)
'''
INJECT = '''import os,time,struct,fcntl,json,sys
from pathlib import Path
p=Path("/run/user/1000/vegas-latency")
for event in Path("/sys/class/input").glob("event*"):
    if (event/"device/name").read_text().strip()=="ILITEK_TDDI": break
else: raise SystemExit("Ilitek absent")
fd=os.open("/dev/input/"+event.name,os.O_RDWR)
def axis(code):
    data=fcntl.ioctl(fd,(2<<30)|(24<<16)|(ord('E')<<8)|(0x40+code),bytes(24))
    return struct.unpack("iiiiii",data)[1:3]
ax,ay=axis(53),axis(54)
x=ax[0]+(ax[1]-ax[0])*1000//10000
y=ay[0]+(ay[1]-ay[0])*8380//10000
def emit(t,c,v): os.write(fd,struct.pack("llHHi",0,0,t,c,v))
with open(p/"injected.jsonl","w",buffering=1) as log:
    for n in range(1,13):
        emit(3,47,0);emit(3,57,1234);emit(3,53,x);emit(3,54,y);emit(1,330,1)
        now=time.monotonic_ns();emit(0,0,0)
        log.write(json.dumps({"n":n,"injected_ns":now})+"\\n")
        time.sleep(float(sys.argv[1]))
        emit(3,57,-1);emit(1,330,0);emit(0,0,0)
        time.sleep(float(sys.argv[2]))
os.close(fd)
'''


def command(script, timeout=30):
    data = OPENER.open(urllib.request.Request(URL, data=script.encode()), timeout=timeout).read().decode()
    if not data.rstrip().endswith('NATIVE_COMMAND_STATUS:0'):
        raise RuntimeError(data[-1500:])
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', default='baseline-30hz')
    parser.add_argument('--interval-ms', type=int, default=500)
    parser.add_argument('--hold-ms', type=int, default=60)
    args = parser.parse_args()
    assert re.fullmatch(r'[a-zA-Z0-9_-]+', args.label)
    assert 10 <= args.hold_ms < args.interval_ms <= 1000
    # Validate geometry before sending a contact; the coordinates target "a".
    hc = 'runuser -u omarchy -- env XDG_RUNTIME_DIR=/run/user/1000 LD_LIBRARY_PATH=/opt/omarchy-android/aquamarine/lib /opt/omarchy-android/hyprland/bin/hyprctl -i 0'
    layers = json.loads(command(hc + ' -j layers\n').split('NATIVE_COMMAND_STATUS:')[0])
    keyboard = [x for m in layers.values() for level in m['levels'].values()
                for x in level if x['namespace'] == 'wvkbd']
    assert len(keyboard) == 1 and all(keyboard[0][k] == v for k, v in
        {'x': 0, 'y': 1110, 'w': 720, 'h': 450}.items()), 'Phone keyboard geometry changed'
    script = 'mkdir -p /run/user/1000/vegas-latency\nrm -f /run/user/1000/vegas-latency/ready\n'
    for name, content in [('client.py', CLIENT), ('inject.py', INJECT)]:
        script += f"printf '%s' '{base64.b64encode(content.encode()).decode()}' | base64 -d >/run/user/1000/vegas-latency/{name}\n"
    script += 'chown -R omarchy:omarchy /run/user/1000/vegas-latency\n'
    launch = 'env WAYLAND_DEBUG=1 foot --app-id=vegas-latency --title="Input timing test" python /run/user/1000/vegas-latency/client.py 2>/run/user/1000/vegas-latency/wayland.log'
    script += hc + " dispatch 'hl.dsp.exec_cmd([=[" + launch + "]=])'\n"
    script += 'for i in {1..50}; do test -f /run/user/1000/vegas-latency/ready && break; sleep .1; done\ntest -f /run/user/1000/vegas-latency/ready\n'
    command(script)
    command(f'python /run/user/1000/vegas-latency/inject.py {args.hold_ms/1000} {(args.interval_ms-args.hold_ms)/1000}\n')
    result = command('echo INJECT_BEGIN\ncat /run/user/1000/vegas-latency/injected.jsonl\necho RECEIVE_BEGIN\ncat /run/user/1000/vegas-latency/received.jsonl\necho WAYLAND_BEGIN\ncat /run/user/1000/vegas-latency/wayland.log\n')
    out = ROOT / 'private/linux-bringup/phone-polish-20261007'
    out.mkdir(parents=True, exist_ok=True)
    (out / (args.label + '-trace.log')).write_text(result)
    a = result.split('INJECT_BEGIN\n')[1].split('RECEIVE_BEGIN')[0]
    b = result.split('RECEIVE_BEGIN\n')[1].split('WAYLAND_BEGIN')[0]
    injected = [json.loads(x) for x in a.splitlines() if x.strip()]
    received = [json.loads(x) for x in b.splitlines() if x.strip()]
    assert len(injected) == len(received) == 12, 'Missing/extra test inputs; repeat without touching phone'
    delays = [(y['received_ns'] - x['injected_ns']) / 1e6 for x,y in zip(injected,received)]
    assert min(delays) >= 0
    report = {'label':args.label,'samples':len(delays),'tap_interval_ms':args.interval_ms,
              'tap_hold_ms':args.hold_ms,'event_to_pty_ms':delays,
              'median_ms':statistics.median(delays),'min_ms':min(delays),'max_ms':max(delays),
              'scope':'synthetic Ilitek input frame to raw-byte receipt in Foot child; excludes physical scan and screen presentation'}
    # Correlate each "a" event with its subsequent main-surface commit and
    # requested callback. This is compositor feedback, not panel light output.
    key_at = commit_at = callback_id = None
    key_to_commit, commit_to_callback = [], []
    for line in result.split('WAYLAND_BEGIN\n')[1].splitlines():
        match = re.match(r'\[(\d+):(\d+):(\d+\.\d+)\]', line)
        if not match:
            continue
        stamp = int(match[1])*3600 + int(match[2])*60 + float(match[3])
        if re.search(r'wl_keyboard#\d+\.key\(\d+, \d+, 30, 1\)', line):
            key_at, commit_at, callback_id = stamp, None, None
        elif key_at is not None and commit_at is None:
            frame = re.search(r'wl_surface#\d+\.frame\(new id wl_callback#(\d+)\)', line)
            if frame:
                callback_id = frame[1]
            if ' -> wl_surface#' in line and '.commit()' in line:
                commit_at = stamp
                key_to_commit.append((stamp-key_at)*1000)
        elif commit_at is not None and callback_id and f'wl_callback#{callback_id}.done(' in line:
            commit_to_callback.append((stamp-commit_at)*1000)
            key_at = commit_at = callback_id = None
    report.update(key_to_commit_ms=key_to_commit, commit_to_callback_ms=commit_to_callback,
                  callback_median_ms=statistics.median(commit_to_callback) if commit_to_callback else None,
                  callback_scope='Foot commit to received compositor frame callback; excludes physical screen presentation')
    (out / (args.label + '.json')).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    main()
