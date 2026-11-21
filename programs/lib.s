# Shared output routines, assembled after each program. Leaf functions:
# they clobber t0-t3, a0-a2 and a7, and leave s-registers and ra alone.

    .text
# putc(a0): write one byte
putc:
    addi  sp, sp, -16
    sb    a0, 0(sp)
    li    a0, 1
    mv    a1, sp
    li    a2, 1
    li    a7, 64
    ecall
    addi  sp, sp, 16
    ret

# print_u32(a0): write a0 as unsigned decimal
print_u32:
    addi  sp, sp, -16
    addi  t0, sp, 12          # digits are written backwards, ending here
    mv    t2, t0
    li    t1, 10
pu_loop:
    remu  t3, a0, t1
    addi  t3, t3, '0'
    addi  t0, t0, -1
    sb    t3, 0(t0)
    divu  a0, a0, t1
    bnez  a0, pu_loop
    li    a0, 1
    mv    a1, t0
    sub   a2, t2, t0
    li    a7, 64
    ecall
    addi  sp, sp, 16
    ret

# print_hex(a0): write a0 as 8 lowercase hex digits
print_hex:
    addi  sp, sp, -16
    li    t1, 8
    addi  t0, sp, 8
ph_loop:
    andi  t2, a0, 15
    li    t3, 10
    blt   t2, t3, ph_digit
    addi  t2, t2, 'a' - 10
    j     ph_store
ph_digit:
    addi  t2, t2, '0'
ph_store:
    addi  t0, t0, -1
    sb    t2, 0(t0)
    srli  a0, a0, 4
    addi  t1, t1, -1
    bnez  t1, ph_loop
    li    a0, 1
    mv    a1, t0
    li    a2, 8
    li    a7, 64
    ecall
    addi  sp, sp, 16
    ret
