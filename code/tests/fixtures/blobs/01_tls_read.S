    .text
    .globl _start
_start:
    // x0 = host-provided output buffer (48 bytes)
    mrs x5, tpidr_el0          // TP
    str x5, [x0, #0]

    ldr x6, [x5]               // slot 0: DTV pointer
    str x6, [x0, #8]

    ldr x6, [x5, #8]           // slot 1: pthread_internal_t*
    str x6, [x0, #16]

    ldr w7, [x6, #768]         // pthread_internal_t::errno_value
    str w7, [x0, #24]

    ldr x6, [x5, #40]          // slot 5: stack guard (TP + 5*8)
    str x6, [x0, #32]

    ldur x6, [x5, #-8]         // slot -1: bionic_tls*
    str x6, [x0, #40]

    mov x0, #0
    ret
