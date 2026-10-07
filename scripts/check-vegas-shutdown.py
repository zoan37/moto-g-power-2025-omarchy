#!/usr/bin/env python3
"""Exercise shutdown decisions with fake sysfs; never reboot or write host config."""
import fcntl
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'port/vegas/root'


def main():
    with tempfile.TemporaryDirectory(prefix='vegas-shutdown-check-') as directory:
        test = Path(directory)
        supplies, runtime = test / 'supplies', test / 'runtime'
        supplies.mkdir(); runtime.mkdir()
        library = test / 'power-source-state'
        original = (SOURCE / 'usr/local/libexec/vegas/power-source-state').read_text()
        library.write_text(original.replace('/sys/class/power_supply', str(supplies)))
        client = test / 'vegas-power'
        client.write_text((SOURCE / 'usr/local/bin/vegas-power').read_text().replace(
            '/usr/local/libexec/vegas/power-source-state', str(library)))
        request = runtime / 'vegas-power-request'
        charger = supplies / 'mtk-master-charger'
        charger.mkdir(); (charger / 'type').write_text('Unknown\n')
        battery = supplies / 'battery'; battery.mkdir()
        (battery / 'type').write_text('Battery\n'); (battery / 'online').write_text('1\n')
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=str(runtime))
        clock = test / 'mock-clock.bash'
        clock.write_text('sleep() { SECONDS=$((SECONDS + 30)); }\n')
        env['BASH_ENV'] = str(clock)

        def state():
            return subprocess.check_output(['bash', str(library)], text=True).strip()

        def client_check(action, succeeds, queued):
            request.unlink(missing_ok=True)
            r = subprocess.run(['bash', str(client), action], env=env, capture_output=True, text=True, timeout=5)
            assert (r.returncode == 0) == succeeds, r.stdout + r.stderr
            assert request.exists() == queued, r.stdout + r.stderr
            if queued:
                assert request.read_text() == action + '\n'
            assert not list(runtime.glob('vegas-power-request.*'))

        (charger / 'online').write_text('1\n')
        assert state() == 'connected'
        client_check('poweroff', False, False)  # Timeout must not queue a later surprise shutdown.
        (charger / 'online').write_text('0\n')
        assert state() == 'disconnected'  # Battery online=1 must not count as external power.
        client_check('poweroff', True, True)
        for invalid in ['-22\n', 'n/a\n', '']:
            (charger / 'online').write_text(invalid)
            assert state() == 'unknown'
            client_check('poweroff', False, False)
        (charger / 'online').unlink()
        assert state() == 'unknown'
        client_check('poweroff', False, False)
        (charger / 'online').write_text('0\n')
        wireless = supplies / 'wireless'; wireless.mkdir()
        (wireless / 'type').write_text('Wireless\n'); (wireless / 'online').write_text('1\n')
        assert state() == 'connected'
        client_check('poweroff', False, False)
        (wireless / 'online').write_text('0\n')
        clock.write_text(f'sleep() {{ SECONDS=$((SECONDS + 1)); printf "0\\n" >"{charger}/online"; }}\n')
        (charger / 'online').write_text('1\n')
        client_check('poweroff', True, True)  # Cable removal progresses to one complete request.
        with (runtime / 'vegas-shutdown-wait.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            client_check('poweroff', False, False)
        (charger / 'online').write_text('1\n')
        client_check('reboot', True, True)
        client_check('fastboot', True, True)
        client_check('invalid', False, False)

        # Run the real root helper in an isolated fake filesystem. Its only
        # exec targets below are recording scripts; no host reboot is possible.
        helper = test / 'helper'; helper.mkdir()
        (helper / 'power-source-state').write_text(library.read_text())
        log = test / 'log'; log.mkdir()
        transition_lock = test / 'transition-lock'
        transition = test / 'power-transition'
        text = (SOURCE / 'usr/local/libexec/vegas/power-transition').read_text()
        text = text.replace('/usr/local/libexec/vegas', str(helper))
        text = text.replace('/run/vegas-power-in-progress', str(transition_lock))
        text = text.replace('/var/log/vegas', str(log))
        text = text.replace('/sys/class/power_supply', str(supplies))
        transition.write_text(text)
        executed = test / 'executed'
        for name in ['busybox', 'to-bootloader']:
            stub = helper / name
            stub.write_text(f'#!/bin/bash\nprintf "%s\\n" "$*" >"{executed}"\n')
            stub.chmod(0o755)
        clock.write_text('sync() { :; }\n')
        for online, expected in [('1', False), ('-22', False), ('0', True)]:
            executed.unlink(missing_ok=True)
            (charger / 'online').write_text(online + '\n')
            r = subprocess.run(['bash', str(transition), 'poweroff'], env=env, capture_output=True, timeout=5)
            assert executed.exists() == expected
            assert (r.returncode == 0) == expected
            if expected:
                assert executed.read_text() == 'poweroff -f\n'
                assert not transition_lock.exists()
        # A missing backend must release its lock so recovery can still run.
        (helper / 'busybox').unlink()
        r = subprocess.run(['bash', str(transition), 'poweroff'], env=env, capture_output=True, timeout=5)
        assert r.returncode != 0 and not transition_lock.exists(), (r.returncode, transition_lock.exists(), r.stderr.decode())
        executed.unlink(missing_ok=True)
        (charger / 'online').write_text('1\n')
        r = subprocess.run(['bash', str(transition), 'fastboot'], env=env, capture_output=True, timeout=5)
        assert r.returncode == 0 and executed.exists()  # Recovery is never gated on unplugging.
    print('Shutdown checks passed: power sources, unknown readings, unplug, timeout, duplicate requests, atomic queue, backend failure and fastboot recovery.')


if __name__ == '__main__':
    main()
