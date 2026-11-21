# Recursive fib(n) for n = 0..24, one per line. Exercises calls, the stack
# and callee-saved registers (about 300k calls).
    .text
_start:
    li    s0, 0
    li    s1, 25
fib_outer:
    mv    a0, s0
    call  fib
    call  print_u32
    li    a0, '\n'
    call  putc
    addi  s0, s0, 1
    blt   s0, s1, fib_outer
    li    a0, 0
    li    a7, 93
    ecall

# fib(a0) -> a0
fib:
    li    t0, 2
    blt   a0, t0, fib_base
    addi  sp, sp, -16
    sw    ra, 12(sp)
    sw    s0, 8(sp)
    sw    a0, 4(sp)
    addi  a0, a0, -1
    call  fib
    mv    s0, a0
    lw    a0, 4(sp)
    addi  a0, a0, -2
    call  fib
    add   a0, a0, s0
    lw    s0, 8(sp)
    lw    ra, 12(sp)
    addi  sp, sp, 16
fib_base:
    ret
