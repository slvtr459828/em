    .text
    .globl em_ctx_switch
    .type em_ctx_switch,%function
em_ctx_switch:
    sub  sp, sp, #176
    stp  x19, x20, [sp, #0]
    stp  x21, x22, [sp, #16]
    stp  x23, x24, [sp, #32]
    stp  x25, x26, [sp, #48]
    stp  x27, x28, [sp, #64]
    stp  x29, x30, [sp, #80]
    stp  d8,  d9,  [sp, #96]
    stp  d10, d11, [sp, #112]
    stp  d12, d13, [sp, #128]
    stp  d14, d15, [sp, #144]
    mrs  x2, fpcr
    str  x2, [sp, #160]
    mov  x3, sp
    str  x3, [x0]
    mov  sp, x1
    ldr  x2, [sp, #160]
    msr  fpcr, x2
    ldp  x19, x20, [sp, #0]
    ldp  x21, x22, [sp, #16]
    ldp  x23, x24, [sp, #32]
    ldp  x25, x26, [sp, #48]
    ldp  x27, x28, [sp, #64]
    ldp  x29, x30, [sp, #80]
    ldp  d8,  d9,  [sp, #96]
    ldp  d10, d11, [sp, #112]
    ldp  d12, d13, [sp, #128]
    ldp  d14, d15, [sp, #144]
    add  sp, sp, #176
    ret
    .size em_ctx_switch,.-em_ctx_switch

    .globl em_tramp
    .type em_tramp,%function
em_tramp:
    bl   em_fiber_main
    brk  #0
    .size em_tramp,.-em_tramp

    .section .note.GNU-stack,"",%progbits
