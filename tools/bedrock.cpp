// bedrock: assemble and run RV32IM programs, and check the machine against
// answers computed independently on the host.
//
//   bedrock run prog.s [more.s ...] [--max-steps N] [--stats]
//   bedrock asm prog.s [more.s ...]        hex listing of the image + symbols
//   bedrock --check                         the table in the README
//   bedrock --bench                         instructions per second
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "bedrock/assembler.hpp"
#include "bedrock/cpu.hpp"
#include "bedrock/encode.hpp"
#include "bedrock/memory.hpp"

#ifndef BEDROCK_PROGRAMS_DIR
#define BEDROCK_PROGRAMS_DIR "programs"
#endif

using namespace bedrock;
using u32 = std::uint32_t;

namespace {

constexpr u32 kMemory = 1u << 20;  // 1 MiB

bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

struct RunResult {
  bool assembled = false;
  std::string errors;
  Halt halt = Halt::Running;
  int exit_code = 0;
  std::string output;
  std::uint64_t retired = 0;
  u32 pc = 0, fault_addr = 0;
};

RunResult run_sources(const std::vector<Source>& sources, std::uint64_t max_steps) {
  RunResult r;
  Program p = assemble(sources);
  if (!p.ok()) {
    r.errors = format_errors(p);
    return r;
  }
  Memory mem(kMemory);
  Cpu cpu(mem);
  if (!load(p, mem, cpu)) {
    r.errors = "image does not fit in memory\n";
    return r;
  }
  r.assembled = true;
  r.halt = cpu.run(max_steps);
  r.exit_code = cpu.exit_code();
  r.output = cpu.output();
  r.retired = cpu.retired();
  r.pc = cpu.pc;
  r.fault_addr = cpu.fault_addr();
  return r;
}

RunResult run_program(const std::string& name, std::uint64_t max_steps = 50'000'000) {
  std::vector<Source> srcs(2);
  for (auto [k, file] : {std::pair{0, name}, std::pair{1, std::string("lib.s")}}) {
    srcs[k].name = std::string(BEDROCK_PROGRAMS_DIR) + "/" + file;
    if (!read_file(srcs[k].name, srcs[k].text)) {
      RunResult r;
      r.errors = "cannot read " + srcs[k].name + "\n";
      return r;
    }
  }
  return run_sources(srcs, max_steps);
}

// ---- host-side expected answers -------------------------------------------

std::string expect_fib() {
  std::string s;
  std::function<u32(u32)> fib = [&](u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); };
  for (u32 n = 0; n < 25; ++n) s += std::to_string(fib(n)) + "\n";
  return s;
}

std::string expect_sieve() {
  const int n = 10000;
  std::vector<bool> composite(n, false);
  u32 count = 0, sum = 0;
  for (int i = 2; i < n; ++i) {
    if (composite[i]) continue;
    ++count;
    sum += static_cast<u32>(i);
    for (long j = static_cast<long>(i) * i; j < n; j += i) composite[static_cast<std::size_t>(j)] = true;
  }
  return std::to_string(count) + " " + std::to_string(sum) + "\n";
}

std::string expect_sort() {
  std::vector<u32> v;
  u32 x = 12345;
  for (int i = 0; i < 300; ++i) {
    x = x * 1103515245u + 12345u;
    v.push_back(x >> 16);
  }
  std::sort(v.begin(), v.end());
  u32 checksum = 0;
  for (u32 i = 0; i < v.size(); ++i) checksum += (i + 1) * v[i];
  return std::to_string(v.front()) + " " + std::to_string(v.back()) + " " + std::to_string(checksum) + " 1\n";
}

std::string expect_muldiv() {
  // Straight from the spec's table and hand arithmetic, not from cpu.cpp.
  const u32 rows[] = {0xfffffffd, 0x00000001, 0xfffffffd, 0xffffffff, 0xffffffff, 0xffffffff, 0x00001234, 0x00001234,
                      0x80000000, 0x00000000, 0x00000000, 0xfffffffe, 0xffffffff, 0x40000000,
                      0x12345678u * 0x9abcdef0u};
  std::string s;
  char buf[16];
  for (u32 r : rows) {
    std::snprintf(buf, sizeof buf, "%08x\n", r);
    s += buf;
  }
  return s;
}

// ---- random single-instruction differential test -------------------------

struct AluOp {
  const char* name;
  char fmt;  // R or I or H
  u32 opc, f3, f7;
  std::function<u32(u32, u32)> ref;
};

std::vector<AluOp> alu_ops() {
  using i32 = std::int32_t;
  using i64 = std::int64_t;
  auto s = [](u32 v) { return static_cast<i64>(static_cast<i32>(v)); };
  auto u = [](u32 v) { return static_cast<i64>(v); };
  auto lo = [](i64 v) { return static_cast<u32>(static_cast<std::uint64_t>(v)); };
  auto hi = [](i64 v) { return static_cast<u32>(static_cast<std::uint64_t>(v) >> 32); };
  return {
      {"add", 'R', 0x33, 0, 0x00, [=](u32 a, u32 b) { return lo(u(a) + u(b)); }},
      {"sub", 'R', 0x33, 0, 0x20, [=](u32 a, u32 b) { return lo(u(a) - u(b)); }},
      {"sll", 'R', 0x33, 1, 0x00, [=](u32 a, u32 b) { return lo(u(a) << (b % 32)); }},
      {"slt", 'R', 0x33, 2, 0x00, [=](u32 a, u32 b) { return u32(s(a) < s(b)); }},
      {"sltu", 'R', 0x33, 3, 0x00, [=](u32 a, u32 b) { return u32(u(a) < u(b)); }},
      {"xor", 'R', 0x33, 4, 0x00, [=](u32 a, u32 b) { return a ^ b; }},
      {"srl", 'R', 0x33, 5, 0x00, [=](u32 a, u32 b) { return lo(u(a) >> (b % 32)); }},
      {"sra", 'R', 0x33, 5, 0x20, [=](u32 a, u32 b) { return lo(s(a) / (i64{1} << (b % 32)) - ((s(a) < 0 && s(a) % (i64{1} << (b % 32)) != 0) ? 1 : 0)); }},
      {"or", 'R', 0x33, 6, 0x00, [=](u32 a, u32 b) { return a | b; }},
      {"and", 'R', 0x33, 7, 0x00, [=](u32 a, u32 b) { return a & b; }},
      {"mul", 'R', 0x33, 0, 0x01, [=](u32 a, u32 b) { return static_cast<u32>(std::uint64_t{a} * b); }},
      {"mulh", 'R', 0x33, 1, 0x01, [=](u32 a, u32 b) { return hi(s(a) * s(b)); }},
      {"mulhsu", 'R', 0x33, 2, 0x01, [=](u32 a, u32 b) { return hi(s(a) * u(b)); }},
      {"mulhu", 'R', 0x33, 3, 0x01, [=](u32 a, u32 b) { return hi(static_cast<i64>(static_cast<std::uint64_t>(a) * b)); }},
      {"div", 'R', 0x33, 4, 0x01, [=](u32 a, u32 b) { return b == 0 ? ~0u : lo(s(b) == -1 ? -s(a) : s(a) / s(b)); }},
      {"divu", 'R', 0x33, 5, 0x01, [=](u32 a, u32 b) { return b == 0 ? ~0u : a / b; }},
      {"rem", 'R', 0x33, 6, 0x01, [=](u32 a, u32 b) { return b == 0 ? a : lo(s(b) == -1 ? 0 : s(a) % s(b)); }},
      {"remu", 'R', 0x33, 7, 0x01, [=](u32 a, u32 b) { return b == 0 ? a : a % b; }},
      {"addi", 'I', 0x13, 0, 0, [=](u32 a, u32 b) { return lo(u(a) + s(b)); }},
      {"slti", 'I', 0x13, 2, 0, [=](u32 a, u32 b) { return u32(s(a) < s(b)); }},
      {"sltiu", 'I', 0x13, 3, 0, [=](u32 a, u32 b) { return u32(a < b); }},
      {"xori", 'I', 0x13, 4, 0, [=](u32 a, u32 b) { return a ^ b; }},
      {"ori", 'I', 0x13, 6, 0, [=](u32 a, u32 b) { return a | b; }},
      {"andi", 'I', 0x13, 7, 0, [=](u32 a, u32 b) { return a & b; }},
      {"slli", 'H', 0x13, 1, 0x00, [=](u32 a, u32 b) { return lo(u(a) << b); }},
      {"srli", 'H', 0x13, 5, 0x00, [=](u32 a, u32 b) { return a >> b; }},
      {"srai", 'H', 0x13, 5, 0x20, [=](u32 a, u32 b) { return lo(s(a) >= 0 ? s(a) >> b : ~(~s(a) >> b)); }},
  };
}

// Returns the number of mismatches; prints the first few.
int alu_fuzz(int trials, bool verbose) {
  std::mt19937 rng(20261121);
  const u32 edges[] = {0, 1, 2, 31, 32, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0x800, 0x7ff};
  auto value = [&]() -> u32 {
    if (rng() % 3 == 0) return edges[rng() % std::size(edges)];
    return static_cast<u32>(rng());
  };
  const auto ops = alu_ops();
  Memory mem(64);
  Cpu cpu(mem);
  int bad = 0;
  for (int t = 0; t < trials; ++t) {
    const AluOp& op = ops[rng() % ops.size()];
    const u32 rd = rng() % 32, rs1 = 1 + rng() % 31, rs2 = 1 + rng() % 31;
    cpu.reset(0);
    for (u32 r = 1; r < 32; ++r) cpu.x[r] = value();
    const u32 a = cpu.x[rs1];
    u32 b = cpu.x[rs2], word = 0;
    if (op.fmt == 'R') {
      word = enc::r(op.f7, rs2, rs1, op.f3, rd, op.opc);
    } else if (op.fmt == 'I') {
      const auto imm = static_cast<std::int32_t>(rng() % 4096) - 2048;
      b = static_cast<u32>(imm);
      word = enc::i(imm, rs1, op.f3, rd, op.opc);
    } else {
      b = rng() % 32;
      word = enc::i(static_cast<std::int32_t>(b | op.f7 << 5), rs1, op.f3, rd, op.opc);
    }
    mem.store(0, 4, word);
    cpu.step();
    const u32 want = rd == 0 ? 0 : op.ref(a, b);
    if (cpu.halt() != Halt::Running || cpu.x[rd] != want) {
      if (verbose && bad < 5)
        std::fprintf(stderr, "  alu-fuzz: %s x%u, a=%08x b=%08x: got %08x want %08x (%s)\n", op.name, rd, a, b,
                     cpu.x[rd], want, to_string(cpu.halt()));
      ++bad;
    }
  }
  return bad;
}

// ---- check table ------------------------------------------------------------

int check() {
  struct Row {
    std::string name;
    bool pass;
    std::string value;
    std::string measures;
  };
  std::vector<Row> rows;

  auto program_row = [&](const std::string& name, const std::string& file, const std::string& want,
                         const std::string& measures) {
    RunResult r = run_program(file);
    const bool pass = r.assembled && r.halt == Halt::Exit && r.exit_code == 0 && r.output == want;
    std::string value = r.assembled ? (std::to_string(r.retired) + " instr") : "asm error";
    if (!pass) {
      std::fprintf(stderr, "%s: %s", name.c_str(), r.errors.c_str());
      if (r.assembled)
        std::fprintf(stderr, "%s: halt=%s exit=%d pc=%08x\n  got:  %s\n  want: %s\n", name.c_str(), to_string(r.halt),
                     r.exit_code, r.pc, r.output.c_str(), want.c_str());
    }
    rows.push_back({name, pass, value, measures});
  };

  program_row("hello", "hello.s", "hello, bedrock\n", "write + exit syscalls, la/li, .ascii");
  program_row("fib-recursive", "fib.s", expect_fib(), "fib(0..24) printed vs host recursion");
  program_row("sieve", "sieve.s", expect_sieve(), "prime count and sum below 10000 vs host");
  program_row("insertion-sort", "sort.s", expect_sort(), "LCG fill, sort, min/max/checksum vs std::sort");
  program_row("muldiv-corners", "muldiv.s", expect_muldiv(), "div/rem by zero, overflow, mulh* vs spec table");

  const int trials = 200000;
  const int bad = alu_fuzz(trials, true);
  rows.push_back({"alu-fuzz", bad == 0, std::to_string(bad) + "/" + std::to_string(trials),
                  "random R/I/shift ALU instructions vs host reference"});

  // Fault paths: each program must stop with the named halt, not run on.
  struct FaultCase {
    const char* name;
    const char* src;
    Halt want;
  };
  const FaultCase faults[] = {
      {"fault-illegal", "_start:\n .word 0xffffffff\n", Halt::IllegalInstruction},
      {"fault-misaligned-jump", "_start:\n li t0, 6\n jr t0\n", Halt::MisalignedFetch},
      {"fault-load", "_start:\n li t0, 0x7ffffff0\n lw t1, 0(t0)\n", Halt::LoadFault},
      {"fault-runaway-pc", "_start:\n li t0, 0x200000\n jr t0\n", Halt::FetchFault},
  };
  int fault_bad = 0;
  for (const auto& f : faults) {
    RunResult r = run_sources({Source{f.name, f.src}}, 1000);
    if (!r.assembled || r.halt != f.want) {
      std::fprintf(stderr, "%s: halt=%s, wanted %s\n%s", f.name, to_string(r.halt), to_string(f.want), r.errors.c_str());
      ++fault_bad;
    }
  }
  rows.push_back({"faults", fault_bad == 0, std::to_string(fault_bad) + "/" + std::to_string(std::size(faults)),
                  "illegal word, misaligned jump, bad load, runaway pc halt"});

  int failed = 0;
  std::printf("%-17s %-6s %16s  %s\n", "check", "result", "value", "measures");
  for (const Row& r : rows) {
    std::printf("%-17s %-6s %16s  %s\n", r.name.c_str(), r.pass ? "PASS" : "FAIL", r.value.c_str(), r.measures.c_str());
    failed += r.pass ? 0 : 1;
  }
  std::printf("\n%d failed\n", failed);
  return failed == 0 ? 0 : 1;
}

int bench() {
  std::string src, lib;
  if (!read_file(std::string(BEDROCK_PROGRAMS_DIR) + "/fib.s", src) ||
      !read_file(std::string(BEDROCK_PROGRAMS_DIR) + "/lib.s", lib)) {
    std::fprintf(stderr, "cannot read programs\n");
    return 1;
  }
  Program p = assemble({Source{"fib.s", src}, Source{"lib.s", lib}});
  Memory mem(kMemory);
  Cpu cpu(mem);
  std::uint64_t total = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int k = 0; k < 10; ++k) {
    load(p, mem, cpu);
    cpu.run(100'000'000);
    total += cpu.retired();
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("fib.s x10: %" PRIu64 " instructions in %.3f s = %.1f MIPS\n", total, s, total / s / 1e6);
  return 0;
}

int usage() {
  std::fprintf(stderr,
               "usage: bedrock run prog.s [more.s ...] [--max-steps N] [--stats]\n"
               "       bedrock asm prog.s [more.s ...]\n"
               "       bedrock --check | --bench\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string cmd = argv[1];
  if (cmd == "--check") return check();
  if (cmd == "--bench") return bench();
  if (cmd != "run" && cmd != "asm") return usage();

  std::vector<Source> srcs;
  std::uint64_t max_steps = 1'000'000'000;
  bool stats = false;
  for (int k = 2; k < argc; ++k) {
    if (!std::strcmp(argv[k], "--max-steps") && k + 1 < argc) {
      max_steps = std::strtoull(argv[++k], nullptr, 10);
    } else if (!std::strcmp(argv[k], "--stats")) {
      stats = true;
    } else {
      Source s{argv[k], ""};
      if (!read_file(s.name, s.text)) {
        std::fprintf(stderr, "cannot read %s\n", argv[k]);
        return 2;
      }
      srcs.push_back(std::move(s));
    }
  }
  if (srcs.empty()) return usage();

  if (cmd == "asm") {
    Program p = assemble(srcs);
    if (!p.ok()) {
      std::fputs(format_errors(p).c_str(), stderr);
      return 1;
    }
    for (u32 a = 0; a + 4 <= p.text_size; a += 4) {
      const u32 w = u32{p.image[a]} | u32{p.image[a + 1]} << 8 | u32{p.image[a + 2]} << 16 | u32{p.image[a + 3]} << 24;
      std::printf("%08x: %08x\n", a, w);
    }
    std::printf("\ntext %u bytes, data at %08x, %zu bytes total, entry %08x\n", p.text_size, p.data_base,
                p.image.size(), p.entry);
    for (const auto& [name, addr] : p.symbols) std::printf("  %08x %s\n", addr, name.c_str());
    return 0;
  }

  RunResult r = run_sources(srcs, max_steps);
  if (!r.assembled) {
    std::fputs(r.errors.c_str(), stderr);
    return 1;
  }
  std::fwrite(r.output.data(), 1, r.output.size(), stdout);
  if (stats) std::fprintf(stderr, "[%" PRIu64 " instructions, %s]\n", r.retired, to_string(r.halt));
  if (r.halt == Halt::Exit) return r.exit_code & 0xff;
  std::fprintf(stderr, "bedrock: %s at pc=%08x (addr %08x) after %" PRIu64 " instructions\n", to_string(r.halt), r.pc,
               r.fault_addr, r.retired);
  return 70;
}
