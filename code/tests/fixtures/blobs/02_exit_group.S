    .text
    .globl _start
_start:
    mov x0, #7
    mov x8, #94                // exit_group
    svc #0
    b .
