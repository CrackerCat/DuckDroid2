// SPDX-License-Identifier: GPL-2.0-only
// M8 trace fixture: a call that leaves the traced region.
//
// x0 on entry is the address of trace_ext (mapped as its own region). The
// blr target is therefore outside the trace range and must appear in bl.log.
// x9 saves the caller's LR so the final ret returns to the return trap.
.text
.global trace_main
trace_main:
    mov x9, x30
    mov x8, x0
    mov x0, #0x5
    blr x8
    add x0, x0, #0x1
    mov x30, x9
    ret
