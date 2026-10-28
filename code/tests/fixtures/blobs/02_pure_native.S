    .text
    .globl _start
_start:
    // x0 = output buffer, x1 = host_add thunk address
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x19, x1                // thunk
    mov x20, x0                // output buffer

    // getpid()
    mov x8, #172
    svc #0
    mov x21, x0
    str x0, [x20, #0]

    // gettid()
    mov x8, #178
    svc #0
    mov x22, x0
    str x0, [x20, #8]

    // mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    mov x0, #0
    mov x1, #4096
    mov x2, #3
    mov x3, #0x22
    mov x4, #-1
    mov x5, #0
    mov x8, #222
    svc #0
    mov x23, x0
    str x0, [x20, #16]

    // store + load through the new mapping
    mov w1, #0x1234
    str w1, [x23]
    ldr w2, [x23]
    str x2, [x20, #24]

    // mprotect(addr, 4096, PROT_READ)
    mov x0, x23
    mov x1, #4096
    mov x2, #1
    mov x8, #226
    svc #0
    str x0, [x20, #32]

    // host_add(pid, tid) through the thunk page
    mov x0, x21
    mov x1, x22
    blr x19
    str x0, [x20, #40]

    // write(1, msg, 3)
    adr x1, msg
    mov x0, #1
    mov x2, #3
    mov x8, #64
    svc #0
    str x0, [x20, #48]

    // munmap(addr, 4096)
    mov x0, x23
    mov x1, #4096
    mov x8, #215
    svc #0
    str x0, [x20, #56]

    // return loaded + host_add + write + munmap + mprotect
    ldr x0, [x20, #24]
    ldr x3, [x20, #40]
    add x0, x0, x3
    ldr x3, [x20, #48]
    add x0, x0, x3
    ldr x3, [x20, #56]
    add x0, x0, x3
    ldr x3, [x20, #32]
    add x0, x0, x3

    ldp x29, x30, [sp], #16
    ret

    .align 2
msg:
    .ascii "m1\n"
