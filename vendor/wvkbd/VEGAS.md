# Vegas keyboard source

Upstream: jjsullivan5196/wvkbd, commit
`e14b53aff4fd1f471add6b21b3885c2cff945509`.

The source here adds immediate submission of pending key feedback behind
`WVKBD_IMMEDIATE_DRAW=1`. Changes are in `main.c`, `drw.c` and `drw.h`.
The repository's `scripts/build-vegas-phone-keyboard.py` validates those base
files, adds the five-row phone layout and bottom safe margin, then cross-builds
the included ARM binary. Its build flags and generated-source manifest are
written to the ignored artifacts directory.

Original copyright notices and GPL v3 terms are preserved in LICENSE,
COPYING and COPYING_WESTON. This directory is not covered by the project's
root MIT license.
