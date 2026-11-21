# M-extension corner cases from the spec's table (unprivileged spec, 7.2):
# division by zero and signed overflow have defined results, no trap.
# Prints one hex word per line.
    .text
_start:
    li    a0, 7
    li    a1, -2
    div   a0, a0, a1          # -3
    call  show
    li    a0, 7
    li    a1, -2
    rem   a0, a0, a1          # 1
    call  show
    li    a0, -7
    li    a1, 2
    div   a0, a0, a1          # -3 (rounds toward zero)
    call  show
    li    a0, -7
    li    a1, 2
    rem   a0, a0, a1          # -1 (sign of dividend)
    call  show
    li    a0, 0x1234
    div   a0, a0, zero        # all ones
    call  show
    li    a0, 0x1234
    divu  a0, a0, zero        # all ones
    call  show
    li    a0, 0x1234
    rem   a0, a0, zero        # dividend
    call  show
    li    a0, 0x1234
    remu  a0, a0, zero        # dividend
    call  show
    li    a0, 0x80000000
    li    a1, -1
    div   a0, a0, a1          # overflow: dividend
    call  show
    li    a0, 0x80000000
    li    a1, -1
    rem   a0, a0, a1          # overflow: 0
    call  show
    li    a0, -1
    li    a1, -1
    mulh  a0, a0, a1          # (-1)(-1) = 1, high word 0
    call  show
    li    a0, -1
    li    a1, -1
    mulhu a0, a0, a1          # (2^32-1)^2 high word = 0xfffffffe
    call  show
    li    a0, -1
    li    a1, -1
    mulhsu a0, a0, a1         # (-1)(2^32-1) high word = 0xffffffff
    call  show
    li    a0, 0x80000000
    li    a1, 0x80000000
    mulh  a0, a0, a1          # (-2^31)^2 = 2^62, high word 0x40000000
    call  show
    li    a0, 0x12345678
    li    a1, 0x9abcdef0
    mul   a0, a0, a1          # low word only
    call  show
    li    a0, 0
    li    a7, 93
    ecall

show:
    addi  sp, sp, -16
    sw    ra, 12(sp)
    call  print_hex
    li    a0, '\n'
    call  putc
    lw    ra, 12(sp)
    addi  sp, sp, 16
    ret
