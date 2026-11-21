# Fill COUNT words from an LCG (x = x*1103515245 + 12345, value = x >> 16),
# insertion-sort them unsigned, then print "<min> <max> <checksum> <sorted>"
# where checksum = sum((i+1) * v[i]) mod 2^32.
    .equ  COUNT, 300

    .text
_start:
    la    s0, values
    li    s1, COUNT
    li    t0, 12345           # x
    li    t4, 1103515245
    li    t1, 0
gen_loop:
    mul   t0, t0, t4
    li    t5, 12345
    add   t0, t0, t5
    srli  t2, t0, 16
    slli  t3, t1, 2
    add   t3, t3, s0
    sw    t2, 0(t3)
    addi  t1, t1, 1
    blt   t1, s1, gen_loop

    li    t1, 1               # i
ins_outer:
    bge   t1, s1, ins_done
    slli  t3, t1, 2
    add   t3, t3, s0
    lw    t2, 0(t3)           # key
    addi  t5, t1, -1          # j
ins_inner:
    bltz  t5, ins_place
    slli  t3, t5, 2
    add   t3, t3, s0
    lw    t6, 0(t3)
    bleu  t6, t2, ins_place
    sw    t6, 4(t3)
    addi  t5, t5, -1
    j     ins_inner
ins_place:
    addi  t3, t5, 1
    slli  t3, t3, 2
    add   t3, t3, s0
    sw    t2, 0(t3)
    addi  t1, t1, 1
    j     ins_outer

ins_done:
    li    s2, 0               # checksum
    li    s3, 1               # sorted?
    li    t1, 0
chk_loop:
    slli  t3, t1, 2
    add   t3, t3, s0
    lw    t2, 0(t3)
    addi  t5, t1, 1
    mul   t6, t2, t5
    add   s2, s2, t6
    bge   t5, s1, chk_next
    lw    t6, 4(t3)
    bleu  t2, t6, chk_next
    li    s3, 0
chk_next:
    addi  t1, t1, 1
    blt   t1, s1, chk_loop

    lw    a0, 0(s0)
    call  print_u32
    li    a0, ' '
    call  putc
    lw    a0, (COUNT - 1) * 4(s0)
    call  print_u32
    li    a0, ' '
    call  putc
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
    .align 2
values:
    .space COUNT * 4
