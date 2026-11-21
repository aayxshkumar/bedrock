// Single-instruction tests. Every expected value here comes from the spec's
// definition, worked out by hand, not from running the emulator.
#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>

#include "bedrock/cpu.hpp"
#include "bedrock/encode.hpp"
#include "bedrock/memory.hpp"

using namespace bedrock;
using u32 = std::uint32_t;

namespace {

constexpr u32 OP = 0x33, IMM = 0x13, LOAD = 0x03, STORE = 0x23, BRANCH = 0x63;

struct Machine {
  Memory mem{4096};
  Cpu cpu{mem};
  explicit Machine(std::initializer_list<u32> words) {
    u32 a = 0;
    for (u32 w : words) {
      mem.store(a, 4, w);
      a += 4;
    }
    cpu.reset(0);
  }
};

}  // namespace

TEST(Cpu, AddWrapsAndX0StaysZero) {
  Machine m({enc::r(0, 2, 1, 0, 3, OP), enc::r(0, 2, 1, 0, 0, OP)});
  m.cpu.x[1] = 0xffffffff;
  m.cpu.x[2] = 2;
  m.cpu.step();
  m.cpu.step();
  EXPECT_EQ(m.cpu.x[3], 1u);
  EXPECT_EQ(m.cpu.x[0], 0u);
  EXPECT_EQ(m.cpu.pc, 8u);
}

TEST(Cpu, ArithmeticVersusLogicalShift) {
  Machine m({enc::r(0x20, 2, 1, 5, 3, OP), enc::r(0x00, 2, 1, 5, 4, OP), enc::i(0x400 | 4, 1, 5, 5, IMM)});
  m.cpu.x[1] = 0x80000000;
  m.cpu.x[2] = 33;  // only the low 5 bits count: shift by 1
  m.cpu.step();
  m.cpu.step();
  m.cpu.step();
  EXPECT_EQ(m.cpu.x[3], 0xc0000000u);  // sra
  EXPECT_EQ(m.cpu.x[4], 0x40000000u);  // srl
  EXPECT_EQ(m.cpu.x[5], 0xf8000000u);  // srai 4
}

TEST(Cpu, SignedVersusUnsignedCompare) {
  Machine m({enc::r(0, 2, 1, 2, 3, OP), enc::r(0, 2, 1, 3, 4, OP), enc::i(-1, 0, 3, 5, IMM)});
  m.cpu.x[1] = 0xffffffff;  // -1 signed, max unsigned
  m.cpu.x[2] = 1;
  m.cpu.step();
  m.cpu.step();
  m.cpu.step();
  EXPECT_EQ(m.cpu.x[3], 1u);  // slt: -1 < 1
  EXPECT_EQ(m.cpu.x[4], 0u);  // sltu: max !< 1
  EXPECT_EQ(m.cpu.x[5], 1u);  // sltiu x0 < 0xffffffff (immediate sign-extended, compared unsigned)
}

TEST(Cpu, LuiAuipc) {
  Machine m({enc::u(0x12345, 1, 0x37), enc::u(0xfffff, 2, 0x17)});
  m.cpu.step();
  m.cpu.step();
  EXPECT_EQ(m.cpu.x[1], 0x12345000u);
  EXPECT_EQ(m.cpu.x[2], 4u + 0xfffff000u);
}

TEST(Cpu, JalLinksAndJumps) {
  Machine m({enc::j(12, 1, 0x6f)});
  m.cpu.step();
  EXPECT_EQ(m.cpu.x[1], 4u);
  EXPECT_EQ(m.cpu.pc, 12u);
}

TEST(Cpu, JalrClearsLowBit) {
  Machine m({enc::i(1, 5, 0, 1, 0x67)});
  m.cpu.x[5] = 16;  // 16 + 1 = 17, low bit cleared -> 16
  m.cpu.step();
  EXPECT_EQ(m.cpu.pc, 16u);
  EXPECT_EQ(m.cpu.x[1], 4u);
}

TEST(Cpu, BranchesTakenAndNot) {
  // blt x1, x2, +8 (taken: -1 < 1), then bltu x1, x2, +8 at 8 (not taken)
  Machine m({enc::b(8, 2, 1, 4, BRANCH), 0, enc::b(8, 2, 1, 6, BRANCH)});
  m.cpu.x[1] = 0xffffffff;
  m.cpu.x[2] = 1;
  m.cpu.step();
  EXPECT_EQ(m.cpu.pc, 8u);
  m.cpu.step();
  EXPECT_EQ(m.cpu.pc, 12u);
}

TEST(Cpu, BackwardBranch) {
  Machine m({0x13, enc::b(-4, 0, 0, 0, BRANCH)});  // nop; beq x0, x0, -4
  m.cpu.step();
  m.cpu.step();
  EXPECT_EQ(m.cpu.pc, 0u);
}

TEST(Cpu, LoadSignAndZeroExtension) {
  Machine m({enc::i(0, 1, 0, 2, LOAD), enc::i(0, 1, 4, 3, LOAD), enc::i(0, 1, 1, 4, LOAD), enc::i(0, 1, 5, 5, LOAD)});
  m.mem.store(256, 4, 0x0000f080);
  m.cpu.x[1] = 256;
  for (int k = 0; k < 4; ++k) m.cpu.step();
  EXPECT_EQ(m.cpu.x[2], 0xffffff80u);  // lb
  EXPECT_EQ(m.cpu.x[3], 0x00000080u);  // lbu
  EXPECT_EQ(m.cpu.x[4], 0xfffff080u);  // lh
  EXPECT_EQ(m.cpu.x[5], 0x0000f080u);  // lhu
}

TEST(Cpu, StoresAreLittleEndianAndSized) {
  Machine m({enc::s(0, 2, 1, 2, STORE), enc::s(5, 3, 1, 0, STORE), enc::s(-2, 3, 1, 1, STORE)});
  m.cpu.x[1] = 256;
  m.cpu.x[2] = 0x11223344;
  m.cpu.x[3] = 0xaabbccdd;
  for (int k = 0; k < 3; ++k) m.cpu.step();
  u32 v = 0;
  m.mem.load(256, 4, v);
  EXPECT_EQ(v, 0x11223344u);
  m.mem.load(260, 4, v);
  EXPECT_EQ(v & 0xff00, 0xdd00u);  // sb at 261
  m.mem.load(254, 2, v);
  EXPECT_EQ(v, 0xccddu);  // sh at 254
}

TEST(Cpu, DivisionCornerCases) {
  // div, divu, rem, remu by zero; div and rem of INT_MIN by -1
  Machine m({enc::r(1, 0, 1, 4, 10, OP), enc::r(1, 0, 1, 5, 11, OP), enc::r(1, 0, 1, 6, 12, OP),
             enc::r(1, 0, 1, 7, 13, OP), enc::r(1, 3, 2, 4, 14, OP), enc::r(1, 3, 2, 6, 15, OP)});
  m.cpu.x[1] = 42;
  m.cpu.x[2] = 0x80000000;
  m.cpu.x[3] = 0xffffffff;
  for (int k = 0; k < 6; ++k) m.cpu.step();
  EXPECT_EQ(m.cpu.x[10], 0xffffffffu);
  EXPECT_EQ(m.cpu.x[11], 0xffffffffu);
  EXPECT_EQ(m.cpu.x[12], 42u);
  EXPECT_EQ(m.cpu.x[13], 42u);
  EXPECT_EQ(m.cpu.x[14], 0x80000000u);
  EXPECT_EQ(m.cpu.x[15], 0u);
}

TEST(Cpu, MultiplyHighVariants) {
  Machine m({enc::r(1, 2, 1, 1, 10, OP), enc::r(1, 2, 1, 2, 11, OP), enc::r(1, 2, 1, 3, 12, OP)});
  m.cpu.x[1] = 0xffffffff;  // -1 / 2^32-1
  m.cpu.x[2] = 2;
  for (int k = 0; k < 3; ++k) m.cpu.step();
  EXPECT_EQ(m.cpu.x[10], 0xffffffffu);  // mulh: -2 -> high word all ones
  EXPECT_EQ(m.cpu.x[11], 0xffffffffu);  // mulhsu: -1 * 2 = -2
  EXPECT_EQ(m.cpu.x[12], 1u);           // mulhu: (2^32-1)*2 = 2^33-2
}

TEST(Cpu, EcallWriteAndExit) {
  Machine m({0x00000073, 0x00000073});
  m.mem.store(512, 4, 0x0a216968);  // "hi!\n"
  m.cpu.x[17] = 64;
  m.cpu.x[10] = 1;
  m.cpu.x[11] = 512;
  m.cpu.x[12] = 4;
  m.cpu.step();
  EXPECT_EQ(m.cpu.output(), "hi!\n");
  EXPECT_EQ(m.cpu.x[10], 4u);
  m.cpu.x[17] = 93;
  m.cpu.x[10] = static_cast<u32>(-3);
  EXPECT_EQ(m.cpu.step(), Halt::Exit);
  EXPECT_EQ(m.cpu.exit_code(), -3);
  EXPECT_EQ(m.cpu.retired(), 2u);
}

TEST(Cpu, FaultsLeavePcOnTheInstruction) {
  Machine m({0x13, 0xffffffff});
  m.cpu.step();
  EXPECT_EQ(m.cpu.step(), Halt::IllegalInstruction);
  EXPECT_EQ(m.cpu.pc, 4u);
  EXPECT_EQ(m.cpu.step(), Halt::IllegalInstruction);  // stays halted
  EXPECT_EQ(m.cpu.retired(), 1u);
}

TEST(Cpu, ReservedEncodingsAreIllegal) {
  for (u32 w : {enc::r(0x20, 1, 1, 1, 1, OP),     // sub-style f7 on sll
                enc::i(0x400 | 1, 1, 1, 1, IMM),  // slli with f7 = 0x20
                enc::i(0, 1, 3, 1, LOAD),         // ld (RV64 only)
                enc::s(0, 1, 1, 3, STORE),        // sd (RV64 only)
                enc::b(8, 1, 1, 2, BRANCH)}) {
    Machine m({w});
    EXPECT_EQ(m.cpu.step(), Halt::IllegalInstruction) << std::hex << w;
  }
}

TEST(Cpu, MisalignedBranchTargetFaults) {
  Machine m({enc::b(6, 0, 0, 0, BRANCH)});
  EXPECT_EQ(m.cpu.step(), Halt::MisalignedFetch);
  EXPECT_EQ(m.cpu.fault_addr(), 6u);
}

TEST(Cpu, OutOfRangeAccessFaults) {
  Machine m({enc::i(0, 1, 2, 2, LOAD)});
  m.cpu.x[1] = 4094;  // a 4-byte load straddling the end of memory
  EXPECT_EQ(m.cpu.step(), Halt::LoadFault);
  EXPECT_EQ(m.cpu.fault_addr(), 4094u);
}

TEST(Cpu, ResetPutsStackAtTopOfMemory) {
  Machine m({});
  EXPECT_EQ(m.cpu.x[2], 4096u);
}
