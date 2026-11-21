# Sieve of Eratosthenes below N; prints "<count> <sum>" of the primes.
    .equ  N, 10000

    .text
_start:
    la    s0, composite
    li    s1, N
    li    t0, 2               # i
sv_outer:
    mul   t1, t0, t0          # j = i*i
    bge   t1, s1, sv_count
    add   t2, s0, t0
    lbu   t3, 0(t2)
    bnez  t3, sv_next
sv_mark:
    add   t2, s0, t1
    li    t3, 1
    sb    t3, 0(t2)
    add   t1, t1, t0
    blt   t1, s1, sv_mark
sv_next:
    addi  t0, t0, 1
    j     sv_outer

sv_count:
    li    s2, 0               # count
    li    s3, 0               # sum
    li    t0, 2
sv_cloop:
    add   t2, s0, t0
    lbu   t3, 0(t2)
    bnez  t3, sv_cskip
    addi  s2, s2, 1
    add   s3, s3, t0
sv_cskip:
    addi  t0, t0, 1
    blt   t0, s1, sv_cloop

    mv    a0, s2
    call  print_u32
    li    a0, ' '
    call  putc
    mv    a0, s3
    call  print_u32
    li    a0, '\n'
    call  putc
    li    a0, 0
    li    a7, 93
    ecall

    .bss
composite:
    .space N
