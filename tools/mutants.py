#!/usr/bin/env python3
"""Plant known machine and assembler bugs one at a time and see what notices.

Each mutant is applied to a scratch copy of the repo (the working tree is
never touched) and built in its own build dir. Then the unit tests and the
machine checks (bedrock --check) run against it.

  python3 tools/mutants.py                 all mutants
  python3 tools/mutants.py sra-logical     one mutant
  python3 tools/mutants.py --list

Extra configure flags can be passed in BEDROCK_CMAKE_ARGS.
Exit code 0 if every mutant was caught, 1 otherwise.
"""
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MUTANTS = {
    "sra-logical": (
        "src/cpu.cpp",
        "else if (f7 == 0x20) result = static_cast<u32>(s32(a) >> shamt);",
        "else if (f7 == 0x20) result = a >> shamt;",
        "srai shifts in zeros instead of copies of the sign bit",
    ),
    "div-by-zero-zero": (
        "src/cpu.cpp",
        "            if (b == 0) result = ~0u;",
        "            if (b == 0) result = 0;",
        "signed division by zero returns 0 instead of all ones",
    ),
    "branch-imm-bit11": (
        "src/cpu.cpp",
        "| (in >> 7 & 1) << 11 |",
        "|",
        "branch decoder drops immediate bit 11",
    ),
    "lh-zero-extend": (
        "src/cpu.cpp",
        "result = (sign && n < 4) ?",
        "result = (sign && n < 2) ?",
        "lh zero-extends instead of sign-extending",
    ),
    "x0-writable": (
        "src/cpu.cpp",
        "if (write && rd != 0) x[rd] = result;",
        "if (write) x[rd] = result;",
        "writes to x0 stick",
    ),
    "jalr-keeps-low-bit": (
        "src/cpu.cpp",
        "const u32 target = (a + imm_i(in)) & ~1u;",
        "const u32 target = (a + imm_i(in));",
        "jalr does not clear bit 0 of the target",
    ),
    "mulhsu-signed-b": (
        "src/cpu.cpp",
        "static_cast<i64>(b)) >> 32",
        "i64{s32(b)}) >> 32",
        "mulhsu treats rs2 as signed",
    ),
    "hi20-no-rounding": (
        "src/assembler.cpp",
        "return ((v + 0x800u) >> 12) & 0xfffffu;",
        "return (v >> 12) & 0xfffffu;",
        "li/la upper part ignores that the low 12 bits are sign-extended",
    ),
    "branch-offset-from-next": (
        "src/assembler.cpp",
        "const i64 off = v->v - offset();",
        "const i64 off = v->v - offset() - 4;",
        "branch and jal offsets measured from the next instruction",
    ),
    "r-type-swapped-sources": (
        "src/assembler.cpp",
        "word(enc::r(op.f7, *rs2, *rs1,",
        "word(enc::r(op.f7, *rs1, *rs2,",
        "R-type encoder swaps rs1 and rs2",
    ),
    "li-always-short": (
        "src/assembler.cpp",
        "fits_signed(v->v, 12)) ? 1 : 2;",
        "fits_signed(v->v, 12)) ? 1 : 1;",
        "li always emits one addi, truncating large constants",
    ),
}


def run(cmd, cwd, timeout=900):
    try:
        return subprocess.run(cmd, cwd=cwd, text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        return subprocess.CompletedProcess(cmd, -999, e.stdout or "", "timeout")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    if args.list:
        for n, m in MUTANTS.items():
            print(f"{n:26} {m[3]}")
        return 0
    names = args.names or list(MUTANTS)
    for n in names:
        if n not in MUTANTS:
            print(f"unknown mutant {n}", file=sys.stderr)
            return 2

    extra = shlex.split(os.environ.get("BEDROCK_CMAKE_ARGS", ""))
    ignore = shutil.ignore_patterns("build*", ".git", "__pycache__")
    survivors = []
    print(f"{'mutant':26} {'unit tests':>11} {'checks':>10}")
    with tempfile.TemporaryDirectory(prefix="bedrock-mutants-") as tmp:
        for name in names:
            path, old, new, what = MUTANTS[name]
            src = os.path.join(tmp, name)
            shutil.copytree(ROOT, src, ignore=ignore)
            target = os.path.join(src, path)
            text = open(target).read()
            if text.count(old) != 1:
                print(f"{name}: anchor not found exactly once in {path}; update tools/mutants.py")
                return 2
            open(target, "w").write(text.replace(old, new))
            build = os.path.join(src, "build")
            c = run(["cmake", "-S", src, "-B", build, "-DCMAKE_BUILD_TYPE=Release", *extra], src)
            b = run(["cmake", "--build", build, "-j", str(args.jobs)], src)
            if c.returncode or b.returncode:
                print(f"{name}: does not compile\n{c.stderr[-1500:]}{b.stdout[-2500:]}{b.stderr[-1500:]}")
                return 2
            t = run([os.path.join(build, "bedrock_tests"), "--gtest_brief=1"], src)
            failed_tests = sorted(set(re.findall(r"\[  FAILED  \] (\S+) \(", t.stdout)))
            unit = "pass" if t.returncode == 0 else (f"{len(failed_tests)} fail" if failed_tests else "crash")
            k = run([os.path.join(build, "bedrock"), "--check"], src)
            failed_checks = [l.split()[0] for l in k.stdout.splitlines() if re.match(r"^\S+\s+FAIL\b", l)]
            checks = "pass" if k.returncode == 0 else (f"{len(failed_checks)} fail" if failed_checks else "crash")
            caught = t.returncode != 0 or k.returncode != 0
            print(f"{name:26} {unit:>11} {checks:>10}   {'' if caught else 'SURVIVED  '}{what}")
            if failed_checks:
                print(f"{'':26} checks: {', '.join(failed_checks)}")
            if failed_tests:
                print(f"{'':26} tests: {', '.join(failed_tests[:4])}{' ...' if len(failed_tests) > 4 else ''}")
            if not caught:
                survivors.append(name)
    if survivors:
        print(f"\n{len(survivors)} mutant(s) survived: {', '.join(survivors)}")
        return 1
    print("\nall mutants caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
