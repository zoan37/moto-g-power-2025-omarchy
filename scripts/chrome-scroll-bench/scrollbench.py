#!/usr/bin/env python3
"""Scripted touch-scroll benchmark over Chrome DevTools (bench Chrome on the phone).

Runs Input.synthesizeScrollGesture on the synthetic feed while tracing, then
reports presented-frame intervals from viz (Display::DrawAndSwap) and Chrome's
own frame reporter verdicts (PipelineReporter: presented / dropped).
"""
import json
import statistics
import sys
import time
import urllib.request

import os
import websocket
MATCH = os.environ.get('BENCH_MATCH', 'bench-feed')

HOST = '192.168.77.1:9223'
label = sys.argv[1] if len(sys.argv) > 1 else 'run'
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3

op = urllib.request.build_opener(urllib.request.ProxyHandler({}))
tabs = json.load(op.open(f'http://{HOST}/json', timeout=10))
page = next(t for t in tabs if t.get('type') == 'page' and MATCH in t.get('url', ''))
ws = websocket.create_connection(page['webSocketDebuggerUrl'].replace('127.0.0.1:9222', HOST),
                                 suppress_origin=True, timeout=120)
browser_ws = None
msg_id = 0


def call(method, params=None, conn=None):
    global msg_id
    conn = conn or ws
    msg_id += 1
    my = msg_id
    conn.send(json.dumps({'id': my, 'method': method, 'params': params or {}}))
    while True:
        m = json.loads(conn.recv())
        if m.get('id') == my:
            if 'error' in m:
                raise RuntimeError(m['error'])
            return m.get('result', {})


_blocked = False


def block_video():
    global _blocked
    if MATCH == 'x.com' and not _blocked:
        call('Network.enable')
        call('Network.setBlockedURLs', {'urls': ['*video.twimg.com*', '*.m3u8*', '*.m4s*', '*.mp4*']})
        _blocked = True


def trace_scroll():
    block_video()
    if MATCH == 'bench-feed':
        call('Runtime.evaluate', {'expression': 'window.scrollTo(0, 1000 + Math.random() * 60000)'})
    time.sleep(1.5)
    vp = call('Runtime.evaluate', {'expression': 'JSON.stringify([innerWidth, innerHeight])', 'returnByValue': True})
    w, h = json.loads(vp['result']['value'])
    call('Tracing.start', {'traceConfig': {'includedCategories': ['benchmark', 'viz', 'cc', 'disabled-by-default-devtools.timeline.frame'],
                                          'recordMode': 'recordAsMuchAsPossible'}})
    time.sleep(0.3)
    t0 = time.time()
    # Real finger path: inject a 0.4 s, 1200 px upward swipe into the touchscreen
    # evdev node on the phone, then let Chrome's fling run out.
    op.open(urllib.request.Request('http://192.168.77.1:8080/cgi-bin/command',
            b'python3 /usr/local/libexec/vegas/swipe.py 540 1900 700 400'), timeout=30).read()
    time.sleep(2.0)
    dur = time.time() - t0
    time.sleep(0.3)
    ws.send(json.dumps({'id': 999999, 'method': 'Tracing.end'}))
    events = []
    while True:
        m = json.loads(ws.recv())
        if m.get('method') == 'Tracing.dataCollected':
            events += m['params']['value']
        elif m.get('method') == 'Tracing.tracingComplete':
            break
    return events, dur


def analyze(events):
    sub = [e for e in events if e.get('name') == 'SubmitCompositorFrameToPresentationCompositorFrame' and e.get('ph') == 'e']
    if not sub:
        return [], [], {}
    renderer = max(set(e['pid'] for e in sub), key=lambda pid: sum(1 for e in sub if e['pid'] == pid))
    # Presentation time of each renderer frame (each is logged once per category).
    swaps = sorted(set(e['ts'] for e in sub if e['pid'] == renderer))
    states = {}
    for e in events:
        if e.get('name') == 'PipelineReporter' and e.get('ph') == 'b' and e.get('pid') == renderer:
            fr = e.get('args', {}).get('frame_reporter') or {}
            if fr.get('scroll_state', 'SCROLL_NONE') == 'SCROLL_NONE' and not fr.get('affects_smoothness'):
                continue
            st = fr.get('state', '?').replace('STATE_', '').lower()
            states[st] = states.get(st, 0) + 1
            if fr.get('checkerboarded_needs_raster') or fr.get('checkerboarded_needs_record'):
                states['checkerboarded'] = states.get('checkerboarded', 0) + 1
    gaps = [(b - a) / 1000 for a, b in zip(swaps, swaps[1:])]
    gaps = [g for g in gaps if g < 100]   # ignore the idle tail after the fling
    return swaps, gaps, states


results = []
for r in range(runs):
    events, dur = trace_scroll()
    swaps, gaps, states = analyze(events)
    if len(gaps) < 5:
        print('too few frames', len(swaps), states)
        continue
    fps = 1000 * len(gaps) / sum(gaps)
    gaps_s = sorted(gaps)
    p = lambda q: gaps_s[min(len(gaps_s) - 1, int(q * len(gaps_s)))]
    vsync = 1000 / float(os.environ.get('BENCH_HZ', '120'))
    late = sum(1 for g in gaps if g > vsync * 1.5)
    res = {'run': r, 'gesture_s': round(dur, 2), 'frames': len(swaps), 'fps': round(fps, 1),
           'median_ms': round(statistics.median(gaps), 1), 'p90_ms': round(p(0.9), 1), 'p99_ms': round(p(0.99), 1),
           'max_ms': round(max(gaps), 1), 'late_pct': round(100 * late / len(gaps), 1),
           'reporter': states}
    results.append(res)
    print(label, json.dumps(res))
if results:
    print(label, 'MEAN fps %.1f  median %.1f ms  p90 %.1f ms  late %.1f%%' % (
        statistics.mean(r['fps'] for r in results), statistics.mean(r['median_ms'] for r in results),
        statistics.mean(r['p90_ms'] for r in results), statistics.mean(r['late_pct'] for r in results)))
