# Remote button pusher for the Moto G Power 2025 (XT2515-1 / vegas)

This is a SuzyQ-style "hands" box for the phone. A Raspberry Pi Pico drives three
9 g servos, and each servo pushes a printed plunger straight onto one side key
(Vol Up, Vol Down, Power). The phone lies screen-up in a printed cradle with
USB-C still plugged in, and a webcam above it shows the screen. An agent on the
laptop runs `phone-buttons.py hold power+voldown 10` or
`phone-buttons.py fastboot --until-fastboot` and checks the result with
`phone-buttons.py snap`.

Status: this is a design. The firmware and host CLI were tested end to end
against a simulated Pico on a pty. The OpenSCAD model renders, and its parts
were checked for interference. **Nothing has been printed or tried on the phone
yet.** The button positions are estimates (see [Measure before printing](#measure-before-printing)).

Files:

| File | What |
| --- | --- |
| `cradle.scad` | Parametric OpenSCAD: `part="cradle"`, `"plungers"`, `"fit_gauge"`, `"assembly"` |
| `firmware/main.py` | MicroPython firmware for Pico / Pico 2 (copy to the board as `main.py`) |
| `phone-buttons.py` | Host CLI (pyserial): press/hold/combos, fastboot and power-cycle macros, calibration, webcam snapshot |

## Button layout (research)

| Fact | Status | Source |
| --- | --- | --- |
| All keys on the **right edge**; power key is the fingerprint reader | Confirmed | [GSMArena review](https://www.gsmarena.com/motorola_moto_g_power_2025-review-2801p2.php), [Motorola hardware page](https://en-us.support.motorola.com/app/answers/detail/a_id/184316/~/about-your-hardware--moto-g-power---2025), [PhoneArena specs](https://www.phonearena.com/phones/Motorola-Moto-G-Power-2025_id12593) |
| Order from the top (camera end): **Vol Up, Vol Down** (one split rocker), then **Power** | Confirmed from GSMArena's side photo (`phone/gsmarena_011.jpg`) | GSMArena review gallery |
| Body 167.2 × 76.8 × 8.7 mm | Spec sheet (PhoneArena lists 166.6 × 77.1) | Motorola / GSMArena |
| Rocker ≈ 20 mm long, starts ≈ 39 mm from the top edge; Vol Up centre ≈ 44, Vol Down ≈ 54 mm | **Estimated** from that photo, ±5 mm | pixel scale from the 8.7 mm frame |
| Power key ≈ 14 mm long, centre ≈ 75 mm from the top edge | **Estimated**, ±5 mm. Android Authority also says the key sits "a bit too high" | same photo |
| Keys ≈ 2.7 mm tall, centred in the frame (≈ 4.4 mm above the back), proud ≈ 0.5 mm | **Estimated** | same photo |
| Rear camera island near the top corner on the button side (when screen-up, it sits under the top-right) | Observed in photo; size estimated | same photo |

## Approaches compared

| Approach | Force / reliability | Phone safety | Removal, noise, heat | Cost / effort | Verdict |
| --- | --- | --- | --- | --- | --- |
| **(a1) Servo + guided plunger (Scotch yoke)** | SG90 stall 1.8 kg·cm at a 13 mm pin is about 13 N available, and 3 N needs about 20 % of it. Servos last tens of thousands of small moves; MG90S metal gears last longer. Spares cost $3 | Tip moves **straight in**, so it never slides on the paint. Force is limited by calibration | Lift the phone out. About 0.3 s of whine, then pulses stop: silent and cold while idle | ~$45–65, one print, **no soldering** | **Recommended** |
| (a2) Servo horn pressing the key directly | Same | Arc motion scrubs the key and frame; needs felt | Same | Easiest | OK as a no-print hack |
| (a3) 5 V push solenoids | Force falls off steeply with air gap and is hard to set at 2–3 N. 1 A at 5 V is 5 W, so a 10 s hold gets hot without PWM hold-down | They bang into the frame | Loud clack | Needs MOSFET drivers and flyback diodes (soldering) | No |
| (b) Linear actuators (Actuonix PQ12 etc.) | Very good, with position feedback | Straight push | Quiet | ~$70+ each and bulky. Cheap "micro linear servos" are too weak | Overkill |
| (c) Open phone, PhotoMOS/opto across key pads | Best possible: no wear, silent, exact timing | Breaks the IP68 glue seal; hair-thin wires on the key flex; voids everything | Phone becomes tethered to a harness | Hard | Only if the phone is ever opened anyway. Use a bidirectional PhotoMOS (e.g. AQY212) per key so pull-up polarity doesn't matter |
| (d) Commercial pushers (SwitchBot Bot) | Fine for one key | Arm swing | 43 mm wide, so they can't sit side by side on a 10 mm rocker | $25–30 each, BLE | See [zero-solder/print alternative](#zero-soldering-zero-printing-alternative) |

A few further ideas that complement the pusher:

- **Release timing lives on the Pico.** Every press or hold is timed by the
  firmware and capped at 120 s, so a crashed agent can't leave Power held.
- **`fastboot --until-fastboot`** holds Power+VolDown and polls `fastboot devices`,
  releasing the moment the phone enumerates. No guessed hold times.
- **Switchable USB power** (a uhubctl-compatible hub) makes a good second
  actuator. With three independent keys plus VBUS control you can also script
  MediaTek BROM/preloader entry for mtkclient (hold both volume keys while VBUS
  comes back). A single-servo "seesaw" on the rocker would save a servo but
  can't press both volume keys at once, so this design keeps three.

## Mechanism

```
 TOP VIEW (phone screen-up; X runs from the top edge toward USB-C)

   x=0 top edge                                                    USB-C end (open)
   ┌──────────────────────────── phone ────────────────────────────┐  ◄── cable stays in
   │                                                               │
   └───────────────[VU][VD]─────[ PWR ]───────────────────────────-┘  right edge
                     ▲   ▲         ▲          plunger noses (move along Y only)
                 ┌───┴───┴──┐   ┌──┴──┐        guide blocks (closed channels)
                 └───┬───┬──┘   └──┬──┘
       S1 shaft o────╯   │   ╭─────╯────o S3 shaft    horn + M2 pin in yoke slot
       (Vol Up servo)    │   │                         (Scotch yoke: plunger
                         ╰───┼─────o S2 shaft          travel = 13 mm × sin θ)
                             (Vol Down servo, one row back)

 SECTION through one key
            phone 8.7 mm      nose  body        yoke      horn ═══o servo shaft
   ┌────────────────────┐◄═══[══|════════]═══[ ○ slot]   M2 pin ↓   │
   └────────────────────┘    guide channel                      ┌──┴──┐ servo hangs
 ══════ floor 17.4 mm above table ═══════════ deck ════════════│     │ in a through
                                                               └─────┘ pocket
```

- The phone sits on a raised floor. Servos hang by their tabs from a deck
  beside it, so each horn is at plunger height. This sets the floor height
  (17.4 mm); `floor_z` is derived from the servo dimensions.
- Each plunger is a 7 × 5 mm bar with a 2 mm-tall nose (narrower than the
  2.7 mm key, so it never lands on the frame). It slides in a closed channel.
- **Scotch yoke:** an M2 screw through the horn's hole about 13 mm from the
  shaft rides in a slot in the plunger's rear plate. ±7° of rotation gives
  about ±1.6 mm of straight travel. At rest the nose sits about 1.5 mm back
  from the key, which leaves room to drop the phone in.
- **Retention:** a top stop, a 5 mm left lip that takes the push reaction, and
  two ribs on the right. The bottom end is open for USB-C, the mic and the
  3.5 mm jack. A finger notch on the left lets you lift the phone out. There is
  a recess for the rear camera island and an optional 1/4"-20 nut trap at the
  top end for a camera arm.
- Footprint about 198 × 144 × 27 mm (174 mm long with `camera_boss=false`).

## Wiring (no soldering)

```
 Laptop USB ── micro-USB ── Raspberry Pi Pico (powered from the laptop)
                               GP2 (pin 4) ──[330 Ω optional]── Power servo   signal (orange)
                               GP3 (pin 5) ──[330 Ω optional]── VolDown servo signal
                               GP4 (pin 6) ──[330 Ω optional]── VolUp servo   signal
                               GND (pin 3/8/38) ─────────────┐
                                                             │  common ground (required)
 5 V 3 A adapter ─ DC jack screw terminal (+) ──┬────────────┼── servo red (V+)  ×3
                                          (−) ──┴─────┬──────┴── servo brown (GND) ×3
                                       1000 µF 16 V electrolytic across +/− at the
                                       breadboard rail (stripe = −)
 Do NOT connect the 5 V adapter to Pico VBUS/VSYS/3V3. Only the grounds are joined.
```

Plug the servo connectors' female headers onto male-male jumpers into the
breadboard. The Pico H's pre-soldered headers go straight into the same
breadboard. 3.3 V logic drives SG90/MG90S signal inputs fine.

### Power budget

| Load | Current @ 5 V |
| --- | --- |
| Pico (from laptop USB) | ~25 mA |
| Servo idle, pulses stopped (normal state) | ~5 mA each |
| Servo moving, light load | 150–300 mA each, for ~0.2 s |
| Servo holding a key (calibrated, small error) | 50–200 mA each |
| Servo stall (mis-calibrated, pushing into the frame) | ~0.7 A (SG90) to ~1 A (MG90S) |
| **Worst case, 3 stalled** | **~3 A** |

A 5 V 3 A brick covers even the worst case. The 1000 µF capacitor absorbs the
start-up surge, and the firmware staggers combo starts by 40 ms. Running the
servos from the laptop's USB 5 V through the Pico's VBUS pin (with the
capacitor) does work for light presses. But a laptop port gives 0.5–0.9 A,
and a sag can reset the Pico mid-hold. That fails safe (the servos go limp),
but the fastboot attempt fails. Use the separate supply.

## Parts list (Amazon US)

Prices are rough (Oct 2026), so check each listing. Items marked * may already be in your parts bin.

| # | Part | Link | ~Price |
| --- | --- | --- | --- |
| 1 | Raspberry Pi Pico, pre-soldered headers + USB cable (Waveshare) | [B0CNXF4PV9](https://www.amazon.com/dp/B0CNXF4PV9) — or the SB Components Pico H kit with breadboard and jumpers, [B0BFJ5SKNZ](https://www.amazon.com/dp/B0BFJ5SKNZ) | $8–15 |
| 2 | MG90S metal-gear micro servos, 4-pack (need 3; your SG90s work for the prototype) | [AUTOUTLET B07C4SMP9Y](https://www.amazon.com/dp/B07C4SMP9Y) or [Beffkkip B07L6FZVT1](https://www.amazon.com/dp/B07L6FZVT1) | $13–17 |
| 3 | 5 V 3 A adapter **with female DC screw-terminal** (ALITOVE) | [B078RXZM4C](https://www.amazon.com/dp/B078RXZM4C) | ~$10 |
| 4* | Dupont jumpers M-M/M-F/F-F (ELEGOO 120 pcs) | [B01EV70C78](https://www.amazon.com/dp/B01EV70C78) | ~$7 |
| 5* | Breadboard (if not in kit 1): ELEGOO 3× 830-point + wires | [B0BZ3MW7BZ](https://www.amazon.com/dp/B0BZ3MW7BZ) | ~$10 |
| 6* | 1000 µF 16 V electrolytic (assortment) | [Swpeet B07KC99W2K](https://www.amazon.com/dp/B07KC99W2K) | ~$10 |
| 7* | M2 screws and nuts (need 3× M2×10 + 3 nuts) | [M2/M3 kit B0BZ85X6QM](https://www.amazon.com/dp/B0BZ85X6QM) | ~$10 |
| 8 | ~120 g PLA or PETG | — | ~$3 |
| | **Core total** | | **≈ $45–65** (≈ $30 if bins cover 4–7) |
| 9 (opt) | Webcam with autofocus: Logitech C920x (UVC, works on Linux) | [B085TFF7M1](https://www.amazon.com/dp/B085TFF7M1) | $55–70 |
| 10 (opt) | Gooseneck desk clamp with 1/4"-20 screw | e.g. [B0GSR41JJC](https://www.amazon.com/dp/B0GSR41JJC). Any well-reviewed 30 cm clamp arm works | $12–20 |
| 11 (opt) | Right-angle USB-C cable (UGREEN, 3.3 ft) only if desk space is tight; a straight cable works | [B07PLYW6R7](https://www.amazon.com/dp/B07PLYW6R7) | ~$10 |
| | **With camera kit** | | **≈ $115–150** |

## Measure before printing

Measure the phone screen-up on a flat table, with no case. Write the numbers
into the top of `cradle.scad`.

1. **`volup_x`, `voldown_x`, `power_x`**: top edge (camera end) to the
   **centre** of each key. For the rocker, use the centre of each half. These
   matter most (±1 mm). The current values are photo estimates (±5 mm).
2. **`vol_half_len`, `power_len`**: key lengths along the edge.
3. **`btn_z`**: height of the key centreline above the table (caliper depth
   rod). This is critical to about ±0.3 mm, because the 2 mm nose has to land
   on a 2.7 mm key.
4. **`key_h`** (key height across the frame) and **`key_proud`** (how far it
   stands out).
5. **`phone_l/w/t`**: real body size (add a skin/case if you will keep one on).
6. **Camera island**: X range from the top edge, Y range from the right edge,
   height above the back (`cam_*`). The recess must clear it so the phone lies flat.
7. **Your servos**: body length/width, tab underside to body bottom
   (`servo_below_deck`), tab underside to horn underside (`horn_above_deck`),
   screw-hole spacing, and the radius of the horn hole you will use as the pin
   (`horn_r`).

Then **print `part="fit_gauge"` first** (about 10 minutes, 1.6 mm strip).
Stand it on its long edge against the phone's right side. The two windows
should frame the rocker and the power key, and the ticks (1/2/3 = volup /
voldown / power) mark where the noses will land. Adjust and reprint until it
fits, then print the cradle.

## Print

```bash
openscad -D 'part="fit_gauge"' -o fit_gauge.stl cradle.scad
openscad -D 'part="cradle"'    -o cradle.stl    cradle.scad
openscad -D 'part="plungers"'  -o plungers.stl  cradle.scad   # already upside-down for printing
openscad cradle.scad            # GUI; default part="assembly" shows phone, plungers, servos
```

PLA or PETG, 0.2 mm layers, 3 walls, 15–20 % infill, no supports (the channels
and nut trap bridge). Plungers: 100 % infill. The cradle needs a bed of at
least 200 mm, or set `camera_boss=false` for 174 mm. If the model asserts
"servo X collides with ...", change `servo_side` or `y_extra` (Vol Down
already sits one row back).

## Assembly and calibration

1. **Firmware.** Hold BOOTSEL, plug the Pico in, and copy the MicroPython UF2
   for [Pico](https://micropython.org/download/RPI_PICO/) or
   [Pico 2](https://micropython.org/download/RPI_PICO2/). Then:
   ```bash
   pipx install mpremote        # or: pip install --user mpremote
   mpremote connect /dev/ttyACM0 cp firmware/main.py :main.py + reset
   sudo pacman -S python-pyserial   # host dependency
   ./phone-buttons.py ping          # -> pong
   ```
2. **udev** (stable name, keeps ModemManager off the port, user access):
   ```
   # /etc/udev/rules.d/70-phone-buttons.rules  (serial from: udevadm info /dev/ttyACM0 | grep SERIAL_SHORT)
   SUBSYSTEM=="tty", ATTRS{idVendor}=="2e8a", ATTRS{serial}=="<PICO_SERIAL>", SYMLINK+="phone-buttons", MODE="0660", GROUP="uucp", ENV{ID_MM_DEVICE_IGNORE}="1"
   ```
   Add yourself to `uucp` if needed, then `sudo udevadm control --reload && sudo udevadm trigger`.
3. **Wire** the breadboard as above, with the 5 V brick unplugged. Check that
   the grounds are joined and the capacitor polarity is right, then plug in.
4. **Centre the servos before fitting the horns:** run `./phone-buttons.py angle volup 90`
   (and the same for `voldown` and `power`). Press each single-arm horn on
   pointing along the phone axis toward its plunger. The Vol Up horn points
   toward the USB end; the Vol Down and Power horns point toward the top.
5. **Make the pins.** Open the horn hole nearest 13 mm from the centre to 2 mm
   (drill, or a hot M2 screw). Push an M2×10 through from the top and tighten
   a nut underneath. The shank below the nut is the pin. Set `horn_r` to the
   hole you used.
6. **Plungers.** Push each one into its channel from the back, nose first. It
   should slide under its own weight; file or sand it if it binds.
7. **Servos.** Feed the wires down through the pocket into the rear wire
   channel. Lower each servo so its pin drops into its plunger's slot, then
   screw the tabs down with the servo's own screws.
8. **Calibrate each key with the phone in the cradle:** `./phone-buttons.py calibrate power`
   - Step with `+`/`-` (or type an angle). Find the angle where the nose sits
     about 1.5 mm back from the key and press `r` (rest).
   - Step toward the phone until the key clicks, add 1–2° and press `p` (press).
     To confirm the click without guessing, run `evtest` on the phone's key
     input over SSH, or watch the screen.
   - `t` test-presses, `q` quits, and `y` saves to the Pico's flash.
   - If the plunger moves the wrong way, the servo direction is reversed:
     swap the signs of the rest and press offsets.
   - Keep the press angle just past the click. Pushing further only makes the
     servo buzz and heat up during long holds.
9. **Soak test** with the phone at the Android home screen or in Linux:
   `for i in $(seq 200); do ./phone-buttons.py press voldown 120; sleep 0.3; done`.
   Then `./phone-buttons.py status`, and check that the plunger tips haven't
   marked the keys.

## Using it (agent recipes)

```bash
./phone-buttons.py press power             # 200 ms tap (screen on/off)
./phone-buttons.py hold power+voldown 10   # combo, released together after 10 s
./phone-buttons.py fastboot --until-fastboot --timeout 30   # recover from "No valid operating system" etc.
./phone-buttons.py menu next 2; ./phone-buttons.py menu select   # bootloader menu: VolDown / VolUp / Power
./phone-buttons.py powercycle              # hold Power 12 s = forced restart
./phone-buttons.py poweron                 # 3 s Power for a phone that is off
./phone-buttons.py down power --max 20; ...; ./phone-buttons.py up power
./phone-buttons.py snap /tmp/screen.jpg --device /dev/video2 --format mjpeg
./phone-buttons.py -v raw status           # raw protocol, with serial trace
```

Exit codes: 0 ok, 1 failed/timeout, 2 usage/connection. A lock file keeps two
agents from talking to the Pico at once. Protocol details are at the top of
`firmware/main.py`.

## Camera

- **Mount:** clamp a gooseneck to the desk edge and hold the webcam about 20 cm
  above the screen, looking straight down. Tilt it about 10° so it doesn't see
  its own reflection or the ceiling lights. Optionally, screw a short ball head
  into the cradle's 1/4"-20 nut trap at the top end. Put the phone's long axis
  along the camera's wide axis and rotate the image in software.
- **Focus:** cheap fixed-focus modules are focused for 0.5 m and beyond. Big
  bootloader text still reads at 20 cm, but small text won't. Either
  unscrew/refocus the lens or use an autofocus UVC camera (C920x).
- **Your 0c45:636b ("USB 2.0 Camera", Sonix)** is flaky on the other machine.
  Before buying anything:
  - Plug it directly into a laptop port, not a hub.
  - Check `v4l2-ctl -d /dev/videoN --list-formats-ext` and capture MJPEG
    (`--format mjpeg`), not raw YUYV, which uses a lot of USB bandwidth.
  - Pin the exposure so the bright screen isn't blown out:
    `v4l2-ctl -d /dev/videoN -c auto_exposure=1 -c exposure_time_absolute=...`
    (control names vary per camera; `v4l2-ctl -L` lists them).
  - If it still drops off the bus (`dmesg` shows disconnects or `-71`
    errors), replace it.

## Zero-soldering, zero-printing alternative

- **The recommended build already needs no soldering:** Pico H with headers,
  breadboard, Dupont jumpers and the DC screw-terminal.
- **No printer:** hot-glue or double-sided-tape the three servos to a scrap
  pine board beside the phone. Let the horns press the keys directly with a
  small felt pad on each tip (approach a2). Hold the phone with two bookends or
  binder clips against a stop strip. It is less precise and scrubs slightly,
  but the firmware, CLI and calibration flow are identical.
- **Off-the-shelf:** a [SwitchBot Bot](https://www.amazon.com/dp/B07B4D9KVX)
  (~$25–30 each, BLE, scriptable from Linux with `pySwitchbot`). Stand the phone
  on its left edge, buttons up, and stick one Bot over Power and another
  (mounted facing the other way) over Vol Down. It is fiddly, it can't fit
  three on this key cluster, and BLE latency rules out precise timing. Fine
  for "hold Power+VolDown 10 s", poor for menu navigation.
- **Rubber band / binder clip:** clamping Vol Down permanently means every
  forced restart lands in the bootloader. That's handy for an unattended
  "always come back to fastboot" setup, but it still needs something to press
  Power. It isn't a remote solution on its own.

## Limitations and cautions

- Button positions and `btn_z` are estimates until measured (see above).
- Servo pots wear. Keep `press` just past the click, and keep spare servos.
- Don't leave a key held for longer than needed. Holding Power+VolDown again
  on the fastboot screen restarts the phone.
- The MediaTek BROM/preloader key combo needs VBUS re-plugged during the hold.
  That needs a switchable USB port, which isn't part of this build.
