# Chrome touch-scroll benchmark (phone)

Repeatable scroll measurements through the real input path (touchscreen evdev → libinput →
Hyprland → Chrome), with frame timing from Chrome's own trace. Results and method:
`notes/chrome-scrolling-20261010.md`.

| File | Runs on | Purpose |
|---|---|---|
| `swipe.py` | phone (`/usr/local/libexec/vegas/swipe.py`) | inject one finger swipe into `ILITEK_TDDI`: `swipe.py X Y0 Y1 MS` |
| `feed/` | phone (`/home/omarchy/bench-feed/`) | synthetic social feed (400 cards, avatars, JPEGs) |
| `start-feed.sh` | phone, as root | fresh `/tmp` profile on the feed, DevTools 9222, `socat` to 192.168.77.1:9223 |
| `start-profile-copy.sh` | phone, as root | same on x.com with a **copy** of the user's profile (only for accounts you may scroll) |
| `scrollbench.py` | laptop (`pip install websocket-client`) | `scrollbench.py LABEL RUNS`; env `BENCH_MATCH` (URL part, default `bench-feed`), `BENCH_HZ` (panel rate for the late-frame threshold) |
| `stages.py` | laptop | mean/median duration of each Chrome pipeline stage over two swipes |
| `lateframes.py` | laptop | stage durations for on-time vs late frames |

Never point it at someone's real timeline without asking: scripted swipes mark posts as seen.
`Input.synthesizeScrollGesture` does not scroll this desktop Chrome; the evdev swipe does.
On x.com the bench blocks video URLs, because autoplaying video (software decode) dominates
otherwise.
