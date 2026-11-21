// RV32IM hart: user-level integer ISA plus multiply/divide. No CSRs, no
// privilege modes, no interrupts yet (that is stage 2 of the roadmap).
//
// Environment calls follow the Linux RISC-V numbers for the two the programs
// need: a7 = 64 write(fd, buf, len), a7 = 93 exit(code).
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "bedrock/memory.hpp"

namespace bedrock {

enum class Halt {
  Running,
  Exit,                // program called exit
  IllegalInstruction,  // undecodable word
  MisalignedFetch,     // jump or branch to an address not divisible by 4
  FetchFault,          // pc outside memory
  LoadFault,           // load outside memory
  StoreFault,          // store outside memory
  Breakpoint,          // ebreak
  BadSyscall,          // ecall with an a7 we don't implement
  StepLimit,           // run() hit max_steps (returned only, never stored)
};

const char* to_string(Halt h);

class Cpu {
 public:
  explicit Cpu(Memory& mem) : mem_(mem) {}

  std::array<std::uint32_t, 32> x{};
  std::uint32_t pc = 0;

  // Zero registers, pc = entry, sp = top of memory (16-byte aligned).
  void reset(std::uint32_t entry);

  // Execute one instruction. On a fault, pc stays on the faulting instruction.
  Halt step();
  Halt run(std::uint64_t max_steps);

  Halt halt() const { return halt_; }
  int exit_code() const { return exit_code_; }
  const std::string& output() const { return output_; }
  std::uint64_t retired() const { return retired_; }
  std::uint32_t fault_addr() const { return fault_addr_; }

 private:
  Halt stop(Halt h, std::uint32_t addr);
  Halt ecall();

  Memory& mem_;
  Halt halt_ = Halt::Running;
  int exit_code_ = 0;
  std::string output_;
  std::uint64_t retired_ = 0;
  std::uint32_t fault_addr_ = 0;
};

}  // namespace bedrock
