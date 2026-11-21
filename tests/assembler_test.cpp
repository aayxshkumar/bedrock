// Encodings checked against GNU as output for the same source lines.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bedrock/assembler.hpp"

using namespace bedrock;
using u32 = std::uint32_t;

namespace {

std::vector<u32> words(const std::string& src) {
  Program p = assemble(src);
  EXPECT_TRUE(p.ok()) << format_errors(p);
  std::vector<u32> out;
  for (u32 a = 0; a + 4 <= p.text_size; a += 4)
    out.push_back(u32{p.image[a]} | u32{p.image[a + 1]} << 8 | u32{p.image[a + 2]} << 16 | u32{p.image[a + 3]} << 24);
  return out;
}

u32 one(const std::string& line) {
  auto w = words(line);
  EXPECT_EQ(w.size(), 1u) << line;
  return w.empty() ? 0 : w[0];
}

std::string first_error(const std::string& src) {
  Program p = assemble(src);
  return p.errors.empty() ? "" : std::to_string(p.errors[0].line) + ": " + p.errors[0].message;
}

}  // namespace

TEST(Assembler, MatchesGnuAs) {
  EXPECT_EQ(one("addi a0, a0, 1"), 0x00150513u);
  EXPECT_EQ(one("add a0, a1, a2"), 0x00c58533u);
  EXPECT_EQ(one("lw a0, 8(sp)"), 0x00812503u);
  EXPECT_EQ(one("sw ra, 12(sp)"), 0x00112623u);
  EXPECT_EQ(one("ret"), 0x00008067u);
  EXPECT_EQ(one("nop"), 0x00000013u);
  EXPECT_EQ(one("ecall"), 0x00000073u);
  EXPECT_EQ(one("lui a0, 0x12345"), 0x12345537u);
  EXPECT_EQ(one("mul a0, a1, a2"), 0x02c58533u);
  EXPECT_EQ(one("srai a0, a0, 3"), 0x40355513u);
  EXPECT_EQ(one("x: jal ra, x"), 0x000000efu);
  EXPECT_EQ(one("addi x5, x6, -2048"), 0x80030293u);
}

TEST(Assembler, BranchOffsetsBothDirections) {
  auto w = words("top: beq a0, a1, fwd\n nop\nfwd: bne a0, zero, top\n");
  ASSERT_EQ(w.size(), 3u);
  EXPECT_EQ(w[0], 0x00b50463u);  // +8
  EXPECT_EQ(w[2], 0xfe051ce3u);  // -8
}

TEST(Assembler, LiPicksOneOrTwoInstructions) {
  EXPECT_EQ(words("li a0, 2047"), (std::vector<u32>{0x7ff00513}));
  EXPECT_EQ(words("li a0, -2048"), (std::vector<u32>{0x80000513}));
  EXPECT_EQ(words("li a0, 0x12345678"), (std::vector<u32>{0x12345537, 0x67850513}));
  // The low 12 bits are negative, so the upper part rounds up.
  EXPECT_EQ(words("li a0, 0x12345fff"), (std::vector<u32>{0x12346537, 0xfff50513}));
}

TEST(Assembler, LaIsPcRelative) {
  Program p = assemble("_start: la a0, msg\n .data\nmsg: .byte 1\n");
  ASSERT_TRUE(p.ok()) << format_errors(p);
  EXPECT_EQ(p.data_base, 16u);
  EXPECT_EQ(p.symbols.at("msg"), 16u);
  EXPECT_EQ(words("_start: la a0, msg\n .data\nmsg: .byte 1\n"),
            (std::vector<u32>{0x00000517, 0x01050513}));  // auipc a0, 0; addi a0, a0, 16
}

TEST(Assembler, DataDirectives) {
  Program p = assemble(".data\nd: .byte 1, -1\n .half 0x1234\n .word 0xdeadbeef\n .asciz \"a\\n\\\"\"\n .align 2\ne:\n");
  ASSERT_TRUE(p.ok()) << format_errors(p);
  const std::vector<std::uint8_t> want = {1, 0xff, 0x34, 0x12, 0xef, 0xbe, 0xad, 0xde, 'a', '\n', '"', 0};
  ASSERT_GE(p.image.size(), want.size());
  EXPECT_EQ(std::vector<std::uint8_t>(p.image.begin(), p.image.begin() + 12), want);
  EXPECT_EQ(p.symbols.at("e"), 12u);
}

TEST(Assembler, ExpressionsAndHiLo) {
  EXPECT_EQ(words(".equ N, 10\n addi a0, zero, N*4 - (2 + 1)"), (std::vector<u32>{0x02500513}));
  EXPECT_EQ(words("lui a0, %hi(0x12345fff)\n addi a0, a0, %lo(0x12345fff)"),
            (std::vector<u32>{0x12346537, 0xfff50513}));
  EXPECT_EQ(one("addi a0, zero, 'A'"), 0x04100513u);
}

TEST(Assembler, EntryIsStart) {
  Program p = assemble("nop\nnop\n_start: nop\n");
  EXPECT_EQ(p.entry, 8u);
}

TEST(Assembler, ErrorsCarryLineNumbers) {
  EXPECT_EQ(first_error("nop\nfrob a0\n"), "2: unknown instruction 'frob'");
  EXPECT_EQ(first_error("add a0, a1, q9"), "1: bad register 'q9'");
  EXPECT_EQ(first_error("addi a0, a0, 2048"), "1: immediate 2048 out of range for 12-bit signed field");
  EXPECT_EQ(first_error("slli a0, a0, 32"), "1: immediate 32 out of range for 5-bit unsigned field");
  EXPECT_EQ(first_error("j nowhere"), "1: undefined symbol 'nowhere'");
  EXPECT_EQ(first_error("a:\na:\n"), "2: duplicate label 'a'");
  EXPECT_EQ(first_error("add a0, a1"), "1: 'add' expects 3 operands, got 2");
  EXPECT_EQ(first_error(".bogus"), "1: unknown directive '.bogus'");
  EXPECT_EQ(first_error(".data\n nop"), "2: instruction outside .text");
}

TEST(Assembler, BranchOutOfRange) {
  std::string src = "beq a0, a1, far\n .space 4096\nfar: nop\n";
  EXPECT_NE(first_error(src).find("out of range"), std::string::npos);
}

TEST(Assembler, CommentsAndStringsWithHashes) {
  Program p = assemble(".data\ns: .ascii \"a#b, c\"  # trailing comment\n");
  ASSERT_TRUE(p.ok()) << format_errors(p);
  EXPECT_EQ(std::string(p.image.end() - 6, p.image.end()), "a#b, c");
}
