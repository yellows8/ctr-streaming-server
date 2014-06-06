.section .init

.arm

.global _start
.global _init
.global setup_fpscr

.type _init STT_FUNC
.type setup_fpscr STT_FUNC

_start:
ldr r1, =__bss_start @ Clear .bss
ldr r2, =__bss_end
mov r3, #0

bss_clr:
cmp r1, r2
beq _start_done
str r3, [r1]
add r1, r1, #4
b bss_clr

_start_done:
bl setup_fpscr

ldr r0, =__libc_init_array @ global constructors
blx r0

mov r0, #0
mov r1, #0
ldr r2, =main
blx r2

_start_end:
b _start_end
.pool

_init:
bx lr

setup_fpscr:
mov r0, #0x3000000
vmsr fpscr, r0
bx lr

