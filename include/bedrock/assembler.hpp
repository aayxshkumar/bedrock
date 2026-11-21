// Two-pass RV32IM assembler. Text is placed at address 0, data after it on a
// 16-byte boundary, and the entry point is `_start` (or 0 if there is none).
//
// Supported: every RV32IM instruction, the common pseudo-instructions (li, la,
// mv, not, neg, seqz, snez, j, jr, call, ret, nop, beqz/bnez/bltz/bgez/blez/
// bgtz, bgt/ble/bgtu/bleu), directives .text .data .rodata .bss .word .half
// .byte .ascii .asciz .string .space .zero .align .equ .set .globl, and
// expressions of numbers, symbols, + - * ( ) with %hi() / %lo().
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "bedrock/cpu.hpp"
#include "bedrock/memory.hpp"

namespace bedrock {

struct Source {
  std::string name;
  std::string text;
};

struct AsmError {
  std::string file;
  int line = 0;
  std::string message;
};

struct Program {
  std::vector<std::uint8_t> image;  // loaded at address 0
  std::uint32_t entry = 0;
  std::uint32_t text_size = 0;
  std::uint32_t data_base = 0;
  std::map<std::string, std::uint32_t> symbols;
  std::vector<AsmError> errors;

  bool ok() const { return errors.empty(); }
};

// Sources are assembled as if concatenated in order (labels are shared).
Program assemble(const std::vector<Source>& sources);
Program assemble(std::string_view text);

std::string format_errors(const Program& p);

// Copy the image into memory and reset the CPU at the entry point.
bool load(const Program& p, Memory& mem, Cpu& cpu);

}  // namespace bedrock
