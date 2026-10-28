// M3 gate: bare-blob pthread_create/pthread_join equivalent.
//
// Handwritten clone(2)/futex(2) usage, no libc. Each iteration:
//   mmap stack + TLS, clone(CLONE_VM|CLONE_THREAD|CLONE_SETTLS|
//   CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID), parent futex-waits on the tid
//   word until the child's CLEARTID clears it, then unmaps.
//
// Output buffer layout (x19):
//   0x30 iterations   0x40 counter     0x50 expected tp   0x58 tp ok
//   0x60 tid ok       0x68 already     0x70 futex ok      0x78 failures
//   0x80 tid word (clone parent_tid + child cleartid)

    .text
    .globl _start
_start:
    mov x19, x0                // output buffer
    mov x20, #0                // iteration
    mov x21, #0                // futex returned 0
    mov x22, #0                // tid word already cleared
    mov x23, #0                // failures

loop:
    add x25, x19, #0x80
    str xzr, [x25]             // tid word = 0

    // stack = mmap(NULL, 0x4000, PROT_READ|PROT_WRITE,
    //              MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    mov x0, #0
    mov x1, #0x4000
    mov x2, #3
    mov x3, #0x22
    mov x4, #-1
    mov x5, #0
    mov x8, #222
    svc #0
    mov x26, x0                // stack base
    add x27, x0, #0x4000       // stack top

    // tls = mmap(NULL, 0x1000, ...)
    mov x0, #0
    mov x1, #0x1000
    mov x2, #3
    mov x3, #0x22
    mov x4, #-1
    mov x5, #0
    mov x8, #222
    svc #0
    mov x24, x0
    str xzr, [x24]             // DTV slot
    str x24, [x24, #8]         // pthread_internal_t* (minimal TCB)
    str x24, [x19, #0x50]      // what the child's tpidr must equal

    // clone(flags, stack_top, &tid, tls, &tid)
    //   arm64 is CONFIG_CLONE_BACKWARDS: x3 = tls, x4 = child_tid.
    //   CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|SETTLS|PARENT_SETTID|
    //   CHILD_CLEARTID = 0x3d0f00
    movz x0, #0x0f00
    movk x0, #0x3d, lsl #16
    mov x1, x27
    mov x2, x25
    mov x3, x24
    mov x4, x25
    mov x8, #220
    svc #0
    cbnz x0, parent

child:
    ldr x1, [x19, #0x40]
    add x1, x1, #1
    str x1, [x19, #0x40]       // counter++

    mrs x2, tpidr_el0
    ldr x3, [x19, #0x50]
    cmp x2, x3
    b.ne child_skip_tp
    ldr x1, [x19, #0x58]
    add x1, x1, #1
    str x1, [x19, #0x58]
child_skip_tp:

    mov x8, #178               // gettid
    svc #0
    ldr w2, [x19, #0x80]       // tid written by CLONE_PARENT_SETTID
    cmp w0, w2
    b.ne child_skip_tid
    ldr x1, [x19, #0x60]
    add x1, x1, #1
    str x1, [x19, #0x60]
child_skip_tid:

    mov x0, #0
    mov x8, #93                // SYS_exit: this thread only
    svc #0
    b .

parent:
    mov x28, x0                // child tid
    ldr w1, [x25]
    cbz w1, join_done          // child already cleared and exited
    mov x0, x25
    mov x1, #0                 // FUTEX_WAIT
    mov x2, x28                // expected = child tid
    mov x3, #0
    mov x8, #98
    svc #0
    cbz x0, futex_ok
    cmp x0, #-11               // -EAGAIN: value changed before we slept
    b.eq already
    add x23, x23, #1
    b join_done
futex_ok:
    add x21, x21, #1
    b join_done
already:
    add x22, x22, #1

join_done:
    mov x0, x26
    mov x1, #0x4000
    mov x8, #215               // munmap(stack)
    svc #0
    mov x0, x24
    mov x1, #0x1000
    mov x8, #215               // munmap(tls)
    svc #0

    add x20, x20, #1
    cmp x20, #100
    b.lt loop

    str x20, [x19, #0x30]
    str x21, [x19, #0x70]
    str x22, [x19, #0x68]
    str x23, [x19, #0x78]
    mov x0, #0
    ret
