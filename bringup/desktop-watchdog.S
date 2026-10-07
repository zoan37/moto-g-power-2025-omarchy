/* Keep recovery alive across switch_root, even if the new init cannot exec. */
.global _start
.text
_start:
    adr x0, delay
    mov x1, #0
    mov x8, #101 /* nanosleep */
    svc #0
    mov x8, #81 /* sync */
    svc #0
    movz x0, #0xdead
    movk x0, #0xfee1, lsl #16
    movz x1, #0x1969
    movk x1, #0x2812, lsl #16
    movz x2, #0xc3d4
    movk x2, #0xa1b2, lsl #16
    adr x3, target
    mov x8, #142 /* reboot(RESTART2) */
    svc #0
    mov x0, #1
    mov x8, #93
    svc #0
.balign 8
delay:
    .quad 180, 0
target:
    .asciz "bootloader"
