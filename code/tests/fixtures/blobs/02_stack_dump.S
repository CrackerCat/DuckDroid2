    .text
    .globl _start
// Three nested frame records, then an intentionally unimplemented syscall.
// Each level stores the return address the unwinder is expected to find into
// the caller-provided out buffer, so the test can compare frame by frame:
//   out[0] = unused
//   out[1] = B, return address in level1 (frame 1)
//   out[2] = A, return address in _start (frame 2)
//   out[3] = -ENOSYS (frame 3 is the host return trap)
_start:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x20, x0                // out buffer, callee-saved across the calls
    bl level1
    str x30, [x20, #16]        // A
    ldp x29, x30, [sp], #16
    ret

level1:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl level2
    str x30, [x20, #8]         // B
    ldp x29, x30, [sp], #16
    ret

level2:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x8, #0                 // io_setup: intentionally unimplemented
    svc #0
    str x0, [x20, #24]         // -ENOSYS
    ldp x29, x30, [sp], #16
    ret
