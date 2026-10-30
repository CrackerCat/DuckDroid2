// SPDX-License-Identifier: GPL-2.0-only
// M8 trace fixture: the out-of-range callee. It is mapped in its own region,
// so trace_main's blr target lands outside the traced range.
.text
.global trace_ext
trace_ext:
    mov x0, #0x7
    ret
