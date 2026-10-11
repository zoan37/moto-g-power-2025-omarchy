#!/usr/bin/env bash
# Attended audio bring-up test for vegas (Moto G Power 2025), run on the phone as root,
# e.g. curl -s --noproxy '*' --data-binary @scripts/test-vegas-audio.sh \
#        http://192.168.77.1:8080/cgi-bin/command
# (the endpoint passes no arguments, so pick steps with STEPS=... in a wrapper line, or run
# it from a root shell on the phone).
#
# Plays sound. Run it only with someone at the phone who agreed to it. Everything is at LOW
# level by default: test tone -24 dBFS, smart amps attenuated 25 dB, headphone amp -10 dB.
#
# Needs (staged beforehand, see notes/audio-20261011.md):
#   $MODDIR/*.ko         stock vendor_dlkm audio modules (list below)
#   /usr/lib/firmware/aw883xx_acf.bin   (vendor/firmware/aw883xx_acf.bin, sha256 c4c49f39...)
#   ucm2 files from port/vegas/root/usr/share/alsa/ucm2 (only for the "ucm" step)
#
# Steps (STEPS, space separated; default "modules controls speaker mic ucm"):
#   modules     insmod the audio chain, create /dev/snd nodes, wait for the card
#   controls    save amixer controls/contents and /proc/asound/pcm to $LOG
#   speaker     both AW88394 amps (Music profile): 1 kHz tone while recording the main mic
#   earpiece    aw_dev_0 alone in Receiver profile (another 10 dB lower)
#   headphones  MT6377 HP amp, only if the jack reports headphones
#   mic         3 s from the main mic (AIN0), RMS/peak per channel
#   headsetmic  3 s from the headset mic (AIN1), only if the jack reports a mic
#   ucm         alsaucm parse + BootSequence + Speaker device via the UCM PCM, low tone
#   acp         spa-acp-tool probe of the card (what PipeWire will accept)
# Env: TONE_DB (dBFS, default -24), AW_ATTEN (aw rx_volume, 1/8 dB steps, default 200 = -25 dB),
#      HP_RAW (MT6377 'Headset Volume' raw, 18 = -10 dB quietest; 8 = 0 dB; 0 = +8 dB),
#      MODDIR (default /opt/vegas-audio/modules), LOG (default /var/log/vegas/audio-test-<time>.txt)
set -uo pipefail

STEPS=${STEPS:-modules controls speaker mic ucm}
TONE_DB=${TONE_DB:--24}
AW_ATTEN=${AW_ATTEN:-200}
HP_RAW=${HP_RAW:-18}
MODDIR=${MODDIR:-/opt/vegas-audio/modules}
LOG=${LOG:-/var/log/vegas/audio-test-$(date +%Y%m%dT%H%M%S).txt}
TMP=$(mktemp -d /tmp/vegas-audio.XXXXXX)
mkdir -p "$(dirname "$LOG")"
exec > >(tee -a "$LOG") 2>&1

# Dependency order from vendor_dlkm modules.dep (mtk-mbox, rpmsg/tinysys IPI, PBM/MDPM,
# throttling and ccci_* are already loaded by the Wi-Fi/GPU bring-up and are skipped).
MODULES=(
	mtk-btcvsd snd-soc-aw883xx mtk-mbox mtk_rpmsg_mbox mtk_tinysys_ipi
	mtk_low_battery_throttling mtk_dynamic_loading_throttling mtk_mdpm mtk_pbm
	adsp mtk-afe-external scp mtk-scp-audiocommon audio_ipi snd-soc-audiodsp-common
	mtk-scp-audio mtk-sp-spk-amp rps_perf ccmni ccci_util_lib ccci_auxadc ccci_md_all
	snd-soc-mtk-common snd-soc-mt6835-afe nvmem-mt635x-efuse mt6377-accdet snd-soc-mt6377 mt6835-mt6377
)
FW=/usr/lib/firmware/aw883xx_acf.bin
FW_SHA=c4c49f390ccc157cdb7098fabcbb8a2c1c0777ae229001aa6a4499860774ea6c

say() { printf '\n== %s\n' "$*"; }
has_step() { [[ " $STEPS " == *" $1 "* ]]; }
card() { awk '/\[mt6835mt6377 *\]/ {print $1; exit}' /proc/asound/cards; }
pcmdev() { # front-end name -> device number on card $C
	awk -F': ' -v c="$(printf '%02d' "$C")" -v n="$1" \
		'index($1, c "-") == 1 && index($2, n " ") == 1 {split($1, a, "-"); print a[2] + 0; exit}' /proc/asound/pcm
}
cset() { amixer -q -c "$C" cset "name=$1" "${@:2}" || echo "  cset FAILED: $1 ${*:2}"; }
cget() { amixer -c "$C" cget "name=$1" 2>/dev/null | awk -F= '/^ *: values=/ {print $2}'; }

make_snd_nodes() {
	mkdir -p /dev/snd
	for d in /sys/class/sound/*/dev; do
		[ -f "$d" ] || continue
		n=$(basename "$(dirname "$d")")
		IFS=: read -r ma mi <"$d"
		[ -e "/dev/snd/$n" ] || mknod -m 660 "/dev/snd/$n" c "$ma" "$mi"
		chgrp audio "/dev/snd/$n" 2>/dev/null
	done
}

# Jack state from the accdet input device (no ALSA jack kcontrol exists on this card).
jack_state() {
	python3 -I - <<'EOF'
import fcntl, glob, os
name = None
for p in glob.glob('/sys/class/input/event*/device/name'):
    if 'Headset Jack' in open(p).read():
        name = p.split('/')[4]
if not name:
    print('nojack'); raise SystemExit
dev = '/dev/input/' + name
if not os.path.exists(dev):
    maj, mi = open(f'/sys/class/input/{name}/dev').read().strip().split(':')
    os.mknod(dev, 0o20660, os.makedev(int(maj), int(mi)))
buf = bytearray(8)
fcntl.ioctl(open(dev, 'rb'), (2 << 30) | (len(buf) << 16) | (0x45 << 8) | 0x1b, buf)
bits = int.from_bytes(buf, 'little')
out = [n for b, n in ((2, 'headphone'), (4, 'microphone'), (6, 'lineout'), (7, 'jack')) if bits >> b & 1]
print(' '.join(out) or 'empty')
EOF
}

tone() { # tone FILE SECONDS DBFS  -> 48 kHz stereo S16 1 kHz sine, 20 ms fades
	python3 -I - "$@" <<'EOF'
import math, struct, sys, wave
path, secs, db = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
rate, n = 48000, int(48000 * float(sys.argv[2]))
amp = 32767 * 10 ** (db / 20)
fade = int(rate * 0.02)
frames = bytearray()
for i in range(n):
    g = min(1.0, i / fade, (n - 1 - i) / fade) if fade else 1.0
    v = int(amp * g * math.sin(2 * math.pi * 1000 * i / rate))
    frames += struct.pack('<hh', v, v)
with wave.open(path, 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(frames))
EOF
}

analyze() { # analyze FILE [TONE_START_S]  -> per channel RMS/peak dBFS; 1 kHz before/during tone
	python3 -I - "$@" <<'EOF'
import array, math, sys, wave
w = wave.open(sys.argv[1])
ch, rate = w.getnchannels(), w.getframerate()
data = array.array('h', w.readframes(w.getnframes()))
start = float(sys.argv[2]) if len(sys.argv) > 2 else None
def db(x): return 20 * math.log10(x / 32768) if x > 0 else -999
def goertzel(s, f=1000.0):
    k = 2 * math.cos(2 * math.pi * f / rate); a = b = 0.0
    for x in s: a, b = x + k * a - b, a
    p = a * a + b * b - k * a * b
    return math.sqrt(max(p, 0)) / max(len(s), 1) * 2
for c in range(ch):
    s = data[c::ch]
    rms = math.sqrt(sum(x * x for x in s) / max(len(s), 1))
    line = f'  ch{c}: {len(s) / rate:.2f} s, RMS {db(rms):.1f} dBFS, peak {db(max((abs(x) for x in s), default=0)):.1f} dBFS'
    if start is not None:
        i0 = int(rate * start)
        base, during = s[: int(rate * 0.4)], s[i0 + int(rate * 0.2): i0 + int(rate * 1.2)]
        gb, gd = goertzel(base), goertzel(during)
        line += f', 1 kHz {db(gb):.1f} -> {db(gd):.1f} dBFS ({"TONE HEARD" if gd > 4 * max(gb, 1) else "no tone"})'
    print(line)
EOF
}

mic_routes() { # main mic AIN0 (builtin_Mic_SingleMic) + ADDA_TO_CAPTURE1
	cset 'UL1_CH1 ADDA_UL_CH1' 1; cset 'UL1_CH2 ADDA_UL_CH2' 1
	cset ADDA_UL_Mux MTKAIF; cset UL_SRC_MUX AMIC; cset UL2_SRC_MUX AMIC
	cset MISO0_MUX UL1_CH1; cset MISO1_MUX UL1_CH1
	cset ADC_L_Mux 'Left Preamplifier'; cset PGA_L_Mux "${1:-AIN0}"
	cset 'PGA1 Volume' 4
}
spk_routes() { # PLAYBACK1_TO_I2S3 + SPK_INIT clock
	cset 'I2S3_CH1 DL1_CH1' 1; cset 'I2S3_CH2 DL1_CH2' 1
	cset I2S3_HD_Mux Low_Jitter
}
aw_atten() { for i in 0 1; do cset "aw_dev_${i}_rx_volume" "$1"; done; }

restore() {
	[ -n "${C:-}" ] || return
	say "restore (Android reset_device_setting + speaker idle state)"
	cset 'HPL Mux' Open; cset 'HPR Mux' Open
	cset ADC_L_Mux Idle; cset ADC_R_Mux Idle; cset PGA_L_Mux None; cset PGA_R_Mux None
	cset MISO0_MUX UL1_CH1; cset MISO1_MUX UL1_CH2
	cset aw_dev_0_prof Music; cset aw_dev_1_prof Music
	cset aw_dev_0_switch Enable; cset aw_dev_1_switch Enable; cset aw_dev_2_switch Disable
	[ -n "${AW0_VOL:-}" ] && { cset aw_dev_0_rx_volume "$AW0_VOL"; cset aw_dev_1_rx_volume "$AW1_VOL"; }
}
trap 'restore; rm -f "$TMP"/*; rmdir "$TMP"' EXIT

say "vegas audio test $(date -Is), steps: $STEPS, tone $TONE_DB dBFS, aw atten $AW_ATTEN/8 dB"
uname -r

if has_step modules; then
	say modules
	if [ -f "$FW" ] && [ "$(sha256sum "$FW" | cut -d' ' -f1)" = "$FW_SHA" ]; then
		echo "firmware $FW ok"
	else
		echo "MISSING or wrong $FW: the aw883xx codecs load it synchronously at card probe;"
		echo "without it there are no aw_dev_* controls and the speaker stays off."
	fi
	for m in "${MODULES[@]}"; do
		[ -d "/sys/module/${m//-/_}" ] && continue
		[ -f "$MODDIR/$m.ko" ] || { echo "missing $MODDIR/$m.ko"; exit 1; }
		echo "insmod $m"
		timeout -k 2 20 insmod "$MODDIR/$m.ko" || echo "  insmod $m FAILED ($?)"
	done
	for _ in $(seq 1 20); do [ -n "$(card)" ] && break; sleep 1; done
	cat /proc/asound/cards
	make_snd_nodes
	ls -l /dev/snd
	dmesg | grep -i -E 'aw883|awinic|mt6835-mt6377|snd_soc|asoc|accdet|btcvsd|scp_audio|deferred' | tail -40
fi

C=$(card)
[ -n "$C" ] || { echo "no mt6835mt6377 card; stopping"; exit 1; }
make_snd_nodes
SPK=$(pcmdev Playback_1); HP=$(pcmdev Playback_2); MIC=$(pcmdev Capture_1)
echo "card $C: Playback_1=$SPK Playback_2=$HP Capture_1=$MIC; jack: $(jack_state)"
AW0_VOL=$(cget aw_dev_0_rx_volume); AW1_VOL=$(cget aw_dev_1_rx_volume)
echo "aw profiles: $(cget aw_dev_0_prof)/$(cget aw_dev_1_prof)/$(cget aw_dev_2_prof)," \
	"switches $(cget aw_dev_0_switch)/$(cget aw_dev_1_switch)/$(cget aw_dev_2_switch), rx_volume $AW0_VOL/$AW1_VOL"

if has_step controls; then
	say controls
	cat /proc/asound/pcm
	amixer -c "$C" controls | wc -l
	amixer -c "$C" contents >"${LOG%.txt}-contents.txt" && echo "amixer contents -> ${LOG%.txt}-contents.txt"
	for n in 'aw_dev_0_prof' 'aw_dev_2_prof' 'DAC In Mux' 'HPL Mux' 'ADC_L_Mux' 'PGA_L_Mux' 'I2S3_HD_Mux' 'Headset Volume' 'PGA1 Volume'; do
		amixer -c "$C" cget "name=$n" | sed 's/^/  /'
	done
fi

tone "$TMP/tone.wav" 1.5 "$TONE_DB"
tone "$TMP/quiet.wav" 1.5 "$((TONE_DB - 10))"
tone "$TMP/silence.wav" 0.3 -200

if has_step speaker; then
	say "speaker: both AW88394, Music profile, aw atten $AW_ATTEN/8 dB, tone $TONE_DB dBFS"
	spk_routes; mic_routes
	cset aw_dev_2_switch Disable
	cset aw_dev_0_switch Enable; cset aw_dev_1_switch Enable
	cset aw_dev_0_prof Music; cset aw_dev_1_prof Music
	# A profile change resets the amp's rx_volume to 0 dB on the next start, so start once
	# with silence first, then attenuate.
	aplay -q -D "plughw:$C,$SPK" "$TMP/silence.wav"
	aw_atten "$AW_ATTEN"
	echo "rx_volume now $(cget aw_dev_0_rx_volume)/$(cget aw_dev_1_rx_volume)"
	arecord -q -D "plughw:$C,$MIC" -f S16_LE -r 48000 -c 2 -d 3 "$TMP/spk-loop.wav" &
	rec=$!
	sleep 0.8
	aplay -D "plughw:$C,$SPK" "$TMP/tone.wav"; echo "aplay rc $?"
	wait $rec
	echo "main mic during the speaker tone (tone starts ~0.8 s):"
	analyze "$TMP/spk-loop.wav" 0.8
	dmesg | grep -i -E 'aw883|awinic' | tail -15
fi

if has_step earpiece; then
	say "earpiece: aw_dev_0 Receiver profile, aw_dev_1 off, tone $((TONE_DB - 10)) dBFS"
	spk_routes; mic_routes
	cset aw_dev_1_switch Disable; cset aw_dev_0_switch Enable; cset aw_dev_0_prof Receiver
	aplay -q -D "plughw:$C,$SPK" "$TMP/silence.wav"
	aw_atten "$AW_ATTEN"
	arecord -q -D "plughw:$C,$MIC" -f S16_LE -r 48000 -c 2 -d 3 "$TMP/rcv-loop.wav" &
	rec=$!
	sleep 0.8
	aplay -D "plughw:$C,$SPK" "$TMP/quiet.wav"; echo "aplay rc $?"
	wait $rec
	analyze "$TMP/rcv-loop.wav" 0.8
	cset aw_dev_1_switch Enable; cset aw_dev_0_prof Music
fi

if has_step headphones; then
	say "headphones: MT6377 HP amp, Headset Volume raw $HP_RAW, tone $((TONE_DB - 10)) dBFS"
	if [[ " $(jack_state) " == *" headphone "* ]]; then
		cset 'ADDA_DL_CH1 DL2_CH1' 1; cset 'ADDA_DL_CH2 DL2_CH2' 1
		cset 'Headset Volume' "$HP_RAW,$HP_RAW"
		cset 'DAC In Mux' 'Normal Path'; cset 'HPL Mux' 'Audio Playback'; cset 'HPR Mux' 'Audio Playback'
		aplay -D "plughw:$C,$HP" "$TMP/quiet.wav"; echo "aplay rc $?"
		cset 'HPL Mux' Open; cset 'HPR Mux' Open
	else
		echo "skipped: jack reports '$(jack_state)'"
	fi
fi

if has_step mic; then
	say "mic: main mic AIN0, PGA 24 dB, 3 s (speak or tap near the bottom mic)"
	mic_routes
	arecord -D "plughw:$C,$MIC" -f S16_LE -r 48000 -c 2 -d 3 "$TMP/mic.wav"; echo "arecord rc $?"
	analyze "$TMP/mic.wav"
	cp "$TMP/mic.wav" "${LOG%.txt}-mic.wav"
fi

if has_step headsetmic; then
	say "headset mic: AIN1, 3 s"
	if [[ " $(jack_state) " == *" microphone "* ]]; then
		mic_routes AIN1
		arecord -D "plughw:$C,$MIC" -f S16_LE -r 48000 -c 2 -d 3 "$TMP/hsmic.wav"; echo "arecord rc $?"
		analyze "$TMP/hsmic.wav"
	else
		echo "skipped: jack reports '$(jack_state)'"
	fi
fi

if has_step ucm; then
	say "ucm"
	if [ -e /usr/share/alsa/ucm2/conf.d/mt6835-mt6377/mt6835-mt6377.conf ]; then
		alsaucm -c "hw:$C" list _verbs && alsaucm -c "hw:$C" list _devices/HiFi
		for d in Speaker Headphones Mic; do
			v=PlaybackPCM; [ $d = Mic ] && v=CapturePCM
			echo "  $d $v = $(alsaucm -c "hw:$C" get "$v/$d/HiFi" 2>&1)"
		done
		# BootSequence + Speaker enable produce no sound by themselves; then attenuate and play.
		alsaucm -c "hw:$C" -b - <<EOF
reset
set _boot ""
set _verb HiFi
set _enadev Speaker
EOF
		echo "boot+enable rc $?"
		aplay -q -D "plughw:$C,$SPK" "$TMP/silence.wav"
		aw_atten "$AW_ATTEN"
		aplay -D "plughw:$C,$SPK" "$TMP/tone.wav"; echo "aplay via UCM routing rc $?"
	else
		echo "ucm2 files not installed; skipped"
	fi
fi

if has_step acp; then
	say "acp (PipeWire's view; opens each PCM, no sound)"
	command -v spa-acp-tool >/dev/null && timeout 30 spa-acp-tool -c "$C" info 2>&1 | grep -E -i 'profile|port|card|error|not supported' | head -60
fi

say "done; log $LOG"
