// SPDX-License-Identifier: GPL-2.0-only
// M8 address-hook fixture. Arguments: x0 = data pointer, x1 = value to store.
//
// Offsets are fixed:
//   0x00 mov x9, x30
//   0x04 mov x8, x0
//   0x08 str x1, [x8]
//   0x0c ldr x2, [x8]
//   0x10 add x2, x2, #0x1
//   0x14 mov x0, x2
//   0x18 mov x30, x9
//   0x1c ret
.text
.global hook_entry
hook_entry:
    mov x9, x30
    mov x8, x0
    str x1, [x8]
    ldr x2, [x8]
    add x2, x2, #0x1
    mov x0, x2
    mov x30, x9
    ret
