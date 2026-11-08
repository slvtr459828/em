    .text
    .globl em_ctx_switch
    .type em_ctx_switch,@function
em_ctx_switch:
    endbr64
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq  $8, %rsp
    stmxcsr (%rsp)
    fnstcw  4(%rsp)
    movq  %rsp, (%rdi)
    movq  %rsi, %rsp
    ldmxcsr (%rsp)
    fldcw   4(%rsp)
    addq  $8, %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret
    .size em_ctx_switch,.-em_ctx_switch

    .globl em_tramp
    .type em_tramp,@function
em_tramp:
    endbr64
    call em_fiber_main
    ud2
    .size em_tramp,.-em_tramp

    .section .note.GNU-stack,"",@progbits
