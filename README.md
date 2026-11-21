# bedrock

A whole computer built from nothing, one layer at a time, in C++20: a RISC-V (RV32IM) CPU emulator, an assembler, and eventually a kernel, a C-subset compiler and a userland that runs on it. The end goal is for the compiler to compile itself *inside* the emulated machine. No layer borrows from an existing toolchain: no GCC cross-compiler, no binutils, no QEMU. Every layer is checked against answers worked out independently (the RISC-V spec, GNU `as` encodings, or the same algorithm run on the host), and the checks that can't pass yet are listed openly.

**Built so far: stage 0 (CPU) and stage 1 (assembler).** Everything after that is the roadmap below.

```
$ ./build/bedrock --check
check             result            value  measures
hello             PASS           10 instr  write + exit syscalls, la/li, .ascii
fib-recursive     PASS      3929003 instr  fib(0..24) printed vs host recursion
sieve             PASS       138156 instr  prime count and sum below 10000 vs host
insertion-sort    PASS       191052 instr  LCG fill, sort, min/max/checksum vs std::sort
muldiv-corners    PASS         1611 instr  div/rem by zero, overflow, mulh* vs spec table
alu-fuzz          PASS           0/200000  random R/I/shift ALU instructions vs host reference
faults            PASS                0/4  illegal word, misaligned jump, bad load, runaway pc halt

0 failed
```

No dependencies beyond googletest, which ships in `third_party/`, so the build needs no network. Requirements: GCC 11+ or Clang 14+, CMake 3.24+, and Python 3 (standard library only) for `tools/mutants.py`. Developed with Apple clang 17. CI covers GCC and Clang on Linux, plus ASan and UBSan.

## Build and run

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bedrock --check                                  # the table above
./build/bedrock run programs/sieve.s programs/lib.s      # assemble + run; exit code is the program's
./build/bedrock run programs/fib.s programs/lib.s --stats
./build/bedrock asm programs/hello.s                     # hex listing and symbol table
./build/bedrock --bench                                  # instructions per second
ctest --test-dir build                                   # unit tests + the checks
python3 tools/mutants.py                                 # plants bugs, reports what catches each
```

Multiple `.s` files are assembled as one unit, so `programs/lib.s` (putc, print_u32, print_hex) can be shared. googletest 1.15.2 is taken from `third_party/` when that copy exists (for offline machines), otherwise from GitHub, pinned to one SHA-256 either way. Sanitizers: `-DBEDROCK_SANITIZERS=address,undefined`.

## The roadmap

| Stage | Layer | Status | How it will be checked |
|---|---|---|---|
| 0 | **RV32IM CPU**: decoder, all 48 RV32IM instructions, faults, `write`/`exit` syscalls | done | hand-derived unit tests, 200k-instruction differential fuzz against a host model, programs whose output is compared with the host |
| 1 | **Assembler**: two passes, labels, pseudo-instructions, data directives, `%hi`/`%lo` | done | encodings compared with GNU `as`, error messages with line numbers, programs end to end |
| 2 | **Privileged architecture**: CSRs, M/S/U modes, trap entry and `mret`, timer interrupt, Sv32 page tables | not started | spec-derived trap tests written as bedrock assembly (every trap cause, `mcause`/`mepc`/`mtval` values, page-fault cases), plus a minimal ELF32 loader so prebuilt `riscv-tests` binaries can run if a copy is packed into `third_party/` |
| 3 | **Kernel** in bedrock assembly: trap handler, round-robin scheduler, `fork`/`exec`/`wait`, a RAM filesystem | not started | deterministic multi-process programs with known interleavings; isolation tests (a process can't touch another's pages) |
| 4 | **C-subset compiler** (ints, pointers, arrays, structs, functions), writing bedrock assembly | not started | differential: compile each test program here and with host clang, and compare the outputs; fuzz with generated programs |
| 5 | **Userland**: a libc subset, a shell, and the assembler and compiler ported to run *on* bedrock | not started | **bootstrap fixed point**: the compiler compiled on bedrock compiles itself, and stage 2 and stage 3 binaries are byte-identical |

Stage 5 is the point of the project: a toolchain that runs on a machine it built itself, where every byte was produced by code in this repo.

## How the CPU works

```
fetch     pc must be 4-aligned and inside memory, else MisalignedFetch / FetchFault
decode    opcode, rd, funct3, rs1, rs2, funct7; immediates sign-extended per format (I S B U J)
execute   one switch on opcode; reserved funct3/funct7 combinations are IllegalInstruction
memory    flat 1 MiB, little-endian; out-of-range access is a Load/StoreFault, never a wrap
commit    rd written unless rd = x0; pc advances; retired count increments
```

- **Faults stop the machine.** The faulting pc and address are kept for the error message. Stage 2 turns faults into traps to a handler.
- **M-extension corner cases** follow the spec's table: division by zero gives all ones (`div`/`divu`) or the dividend (`rem`/`remu`), and `INT_MIN / -1` gives `INT_MIN` with remainder 0. None of these trap.
- **Syscalls** use the Linux RISC-V numbers for the two implemented: `a7 = 64` write and `a7 = 93` exit. The `write` file descriptor is ignored and all output is captured.
- **Speed:** 211 MIPS running `fib.s` on an Apple M2 (`--bench`). It's a plain switch interpreter that decodes every instruction each time it runs, with no caching.

## How the assembler works

Pass 1 walks every statement to give each label an address. Pass 2 encodes with all symbols known. Text is placed at 0, data on the next 16-byte boundary, and execution starts at `_start`. A statement's size must be identical in both passes, so any size that depends on a value is decided once, in pass 1:

- `li` is one `addi` when the constant is known in pass 1 and fits in 12 bits. Otherwise it is `lui` + `addi`, with the upper part rounded so it survives the sign-extended low 12 bits.
- `.space` and `.align` need a constant that is already known in pass 1.

## Known gaps

1. **No privileged architecture.** There are no CSRs and no traps, so nothing can catch a fault. That is stage 2.
2. **Not run against the official RISC-V test suites.** Every expected value here was derived by hand from the spec, or computed on the host. The official suites are assembly that has to be built with a RISC-V GCC cross-compiler, which this project deliberately doesn't use, so they can only come in as prebuilt binaries through the ELF loader planned for stage 2.
3. **`li` with a symbol that is defined later, or that lives in data, always takes two instructions**, even when one would do (`hello.s` shows this). `call` is a single `jal`, so it only reaches ±1 MiB, which is the whole memory today.
4. **No C (compressed) or A (atomic) extensions.** Code from a real compiler would need `-march=rv32im`. That doesn't matter while every program is written here.
5. **Unit-test blind spot:** a `mulhsu` that treats rs2 as signed passes every unit test. Only `--check` catches it (see below).

## What catches what

`tools/mutants.py` plants each bug in a scratch copy, then runs the unit tests and `--check`:

| Mutant | Bug | Unit tests | Checks |
|---|---|---|---|
| sra-logical | `srai` shifts in zeros | 1 fail | 1 fail |
| div-by-zero-zero | `div x, 0` returns 0 | 1 fail | 2 fail |
| branch-imm-bit11 | branch decoder drops immediate bit 11 | 1 fail | 4 fail |
| lh-zero-extend | `lh` zero-extends | 1 fail | pass |
| x0-writable | writes to x0 stick | 1 fail | 5 fail |
| jalr-keeps-low-bit | `jalr` doesn't clear bit 0 | 1 fail | pass |
| mulhsu-signed-b | `mulhsu` treats rs2 as signed | **pass** | 2 fail |
| hi20-no-rounding | `li`/`la` upper part not rounded | 2 fail | 2 fail |
| branch-offset-from-next | offsets measured from pc+4 | 2 fail | 4 fail |
| r-type-swapped-sources | encoder swaps rs1/rs2 | 1 fail | 4 fail |
| li-always-short | `li` truncates to 12 bits | 1 fail | 4 fail |

All 11 are caught. The programs don't use `lh` or misaligned `jalr` targets, so those two depend on the unit tests alone.

## Layout

```
include/bedrock/  encode.hpp (instruction formats)  memory.hpp  cpu.hpp  assembler.hpp
src/              cpu.cpp  assembler.cpp
tools/            bedrock.cpp (CLI, --check, --bench)  mutants.py
tests/            cpu_test.cpp  assembler_test.cpp
programs/         hello.s fib.s sieve.s sort.s muldiv.s  lib.s (shared output routines)
```

Reference: *The RISC-V Instruction Set Manual, Volume I: Unprivileged ISA* (20191213).
