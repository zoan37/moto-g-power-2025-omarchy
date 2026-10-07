#!/usr/bin/env python3
"""Build an optimized ARM phone keyboard without changing the reviewed source."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'third_party/wvkbd'
BUILD = ROOT / 'artifacts/vegas-linux-bringup/phone-keyboard'
SYSROOT = ROOT.parent / 'fire-hd8-omarchy/working/desktop/rootfs'
EXPECTED = {
    'main.c': '14ad95ea0d5ff1ef592c1c294e1f0c40cc69704a2ef4524263dec8c24d75535f',
    'drw.c': '7696ef46fdb6914336ccb21f06a00b8ae3ce45cad900f81b7e01c9edc83152f0',
    'drw.h': '5da6e1cabdb0e11a5c0a9b8bd4f77925e558bbf0af35b1e0a526edd98a87a3db',
    'keyboard.c': '7e04eeb099f33d63ca2bebbe6917f3c0d6b75a33553c56aafd938c8ba8e3bc48',
    'keyboard.h': '173aa4d8bdfef39883b5efd0d339bdffd961d4823efd47203825c0f666e78ee4',
    'layout.mobintl.h': 'fc41262258523177ecd4257ae1881e9917c5cd00643667db0934de97cbc67d3f',
}


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError('Keyboard source no longer matches the reviewed patch')
    return text.replace(before, after, 1)


def main():
    for name, expected in EXPECTED.items():
        if hashlib.sha256((SOURCE / name).read_bytes()).hexdigest() != expected:
            raise SystemExit(f'Reviewed keyboard source changed: {name}')
    BUILD.mkdir(parents=True, exist_ok=True)
    work = BUILD / 'source'
    if work.exists():
        shutil.rmtree(work)
    shutil.copytree(SOURCE, work, ignore=shutil.ignore_patterns('.git', 'build-*', 'wvkbd-mobintl'))
    layout_file = work / 'layout.mobintl.h'
    layout = layout_file.read_text()
    simple = re.search(r'static struct key keys_simple\[\] = \{(.*?)\n\};', layout, re.S)[1]
    digits = '\n'.join(
        f'  {{"{digit}", "{shift}", 1.0, Code, KEY_{digit}}},'
        for digit, shift in zip('1234567890', '!@#$%^&*()'))
    phone = digits + '\n  {"", "", 0.0, EndRow},\n' + simple
    phone = phone.replace('{"⌨͕", "⌨͔", 1.0, NextLayer', '{"?123", "ABC", 1.0, NextLayer')
    phone = phone.replace('{"Cmp", "Cmp", 1.0, Compose', '{"Esc", "Esc", 1.0, Code, KEY_ESC')
    layout = replace_once(layout, '\tSimple,', '\tSimple,\n\tPhone,')
    layout = replace_once(layout, 'static struct key keys_full[], keys_full_wide[]', 'static struct key keys_phone[];\nstatic struct key keys_full[], keys_full_wide[]')
    layout = replace_once(layout, '  [Simple] =', '  [Phone] = {keys_phone, "latin", "phone", true},\n  [Simple] =')
    layout += '\n/* Vegas: a number row plus the four-row mobile layout. */\nstatic struct key keys_phone[] = {\n' + phone + '\n};\n'
    layout_file.write_text(layout)
    main_file = work / 'main.c'
    text = main_file.read_text()
    text = replace_once(text, 'static uint32_t height, normal_height, landscape_height;',
                        'static uint32_t height, normal_height, landscape_height;\nstatic uint32_t bottom_margin;')
    text = replace_once(text, '    zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);',
                        '    zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);\n'
                        '    zwlr_layer_surface_v1_set_margin(layer_surface, 0, 0, bottom_margin, 0);')
    text = replace_once(text, '        } else if (!strcmp(argv[i], "-H")) {',
                        '        } else if (!strcmp(argv[i], "--margin-bottom")) {\n'
                        '            if (i >= argc - 1) { die("Missing bottom margin\\n"); }\n'
                        '            char *end;\n'
                        '            long margin = strtol(argv[++i], &end, 10);\n'
                        '            if (!argv[i][0] || *end || margin < 0 || margin > 128) {\n'
                        '                die("Bottom margin must be 0..128 logical pixels\\n");\n'
                        '            }\n'
                        '            bottom_margin = (uint32_t)margin;\n'
                        '        } else if (!strcmp(argv[i], "-H")) {')
    text = replace_once(text, '    fprintf(stderr, "  -D          - Enable debug\\n");',
                        '    fprintf(stderr, "  --margin-bottom [int] - Bottom safe area, 0..128 logical pixels\\n");\n'
                        '    fprintf(stderr, "  -D          - Enable debug\\n");')
    main_file.write_text(text)
    env = os.environ.copy()
    env['PKG_CONFIG_SYSROOT_DIR'] = str(SYSROOT)
    env['PKG_CONFIG_LIBDIR'] = f'{SYSROOT}/usr/lib/pkgconfig:{SYSROOT}/usr/share/pkgconfig'
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', 'wayland-client', 'xkbcommon', 'pangocairo'], env=env, text=True))
    libs = shlex.split(subprocess.check_output(
        ['pkg-config', '--libs', 'wayland-client', 'xkbcommon', 'pangocairo'], env=env, text=True))
    cc = f'clang --target=aarch64-linux-gnu --sysroot={SYSROOT} -fuse-ld=lld -nostdlib -no-pie'
    cflags = '-O2 -DVERSION=\\"0.21-vegas\\" -D_XOPEN_SOURCE=700 -std=gnu99 -Wall -DWITH_WAYLAND_SHM -DLAYOUT=\\"layout.mobintl.h\\" -DKEYMAP=\\"keymap.mobintl.h\\" ' + shlex.join(flags)
    ldflags = shlex.join([str(SYSROOT / 'usr/lib/crt1.o'), str(SYSROOT / 'usr/lib/crti.o')])
    ldflags += ' ' + shlex.join(libs + ['-lm', '-lutil', '-lrt', '-lc', str(SYSROOT / 'usr/lib/crtn.o'), '-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1'])
    command = ['make', '-j4', 'wvkbd-mobintl', f'CC={cc}', f'CFLAGS={cflags}', f'LDFLAGS={ldflags}']
    result = subprocess.run(command, cwd=work, capture_output=True, text=True)
    (BUILD / 'build.log').write_text(result.stdout + result.stderr)
    (BUILD / 'build-command.json').write_text(json.dumps(command, indent=2) + '\n')
    result.check_returncode()
    output = ROOT / 'port/vegas/root/usr/local/bin/wvkbd-mobintl'
    shutil.copy2(work / 'wvkbd-mobintl', output)
    output.chmod(0o755)
    check = subprocess.run(['qemu-aarch64-static', '-L', str(SYSROOT), str(output), '--help'], capture_output=True, text=True)
    check.check_returncode()
    assert '--margin-bottom' in check.stderr
    # Exercise the new parser before help exits; help alone never reaches it.
    parser_check = subprocess.run(
        ['qemu-aarch64-static', '-L', str(SYSROOT), str(output),
         '--margin-bottom', '32', '--help'], capture_output=True, text=True)
    parser_check.check_returncode()
    for invalid in ['-1', '129', 'oops']:
        rejected = subprocess.run(
            ['qemu-aarch64-static', '-L', str(SYSROOT), str(output),
             '--margin-bottom', invalid, '--help'], capture_output=True, text=True)
        assert rejected.returncode == 1 and 'Bottom margin must be' in rejected.stderr
    layers = subprocess.run(['qemu-aarch64-static', '-L', str(SYSROOT), str(output), '--list-layers'], capture_output=True, text=True)
    layers.check_returncode()
    assert 'phone' in layers.stdout + layers.stderr
    manifest = {'base_commit': 'e14b53aff4fd1f471add6b21b3885c2cff945509',
                'base_files': EXPECTED, 'optimization': '-O2', 'phone_rows': 5,
                'immediate_feedback_preserved': True, 'arm_help_and_layers_checked': True,
                'tested_on_phone': False, 'sha256': hashlib.sha256(output.read_bytes()).hexdigest()}
    (BUILD / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Built optimized ARM keyboard: five-row phone layout and bottom safe area; hardware test pending.')


if __name__ == '__main__':
    main()
