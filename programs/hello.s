# The smallest whole program: one write, one exit.
    .text
_start:
    li    a0, 1
    la    a1, msg
    li    a2, msg_end - msg
    li    a7, 64
    ecall
    li    a0, 0
    li    a7, 93
    ecall

    .data
msg:
    .ascii "hello, bedrock\n"
msg_end:
