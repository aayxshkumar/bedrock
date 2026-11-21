#include "bedrock/cpu.hpp"

#include <cstdint>

namespace bedrock {

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;

constexpr u32 sext(u32 value, int bits) {
  const u32 m = 1u << (bits - 1);
  value &= (bits == 32) ? ~0u : ((1u << bits) - 1);
  return (value ^ m) - m;
}

constexpr u32 imm_i(u32 in) { return sext(in >> 20, 12); }
constexpr u32 imm_s(u32 in) { return sext((in >> 25) << 5 | (in >> 7 & 0x1f), 12); }
constexpr u32 imm_b(u32 in) {
  return sext((in >> 31) << 12 | (in >> 7 & 1) << 11 | (in >> 25 & 0x3f) << 5 | (in >> 8 & 0xf) << 1, 13);
}
constexpr u32 imm_j(u32 in) {
  return sext((in >> 31) << 20 | (in & 0xff000) | (in >> 20 & 1) << 11 | (in >> 21 & 0x3ff) << 1, 21);
}

constexpr i32 s32(u32 v) { return static_cast<i32>(v); }

constexpr u32 kIntMin = 0x80000000u;

}  // namespace

const char* to_string(Halt h) {
  switch (h) {
    case Halt::Running: return "running";
    case Halt::Exit: return "exit";
    case Halt::IllegalInstruction: return "illegal instruction";
    case Halt::MisalignedFetch: return "misaligned instruction fetch";
    case Halt::FetchFault: return "instruction fetch fault";
    case Halt::LoadFault: return "load fault";
    case Halt::StoreFault: return "store fault";
    case Halt::Breakpoint: return "breakpoint";
    case Halt::BadSyscall: return "unknown syscall";
    case Halt::StepLimit: return "step limit";
  }
  return "?";
}

void Cpu::reset(u32 entry) {
  x.fill(0);
  x[2] = mem_.size() & ~15u;
  pc = entry;
  halt_ = Halt::Running;
  exit_code_ = 0;
  output_.clear();
  retired_ = 0;
  fault_addr_ = 0;
}

Halt Cpu::stop(Halt h, u32 addr) {
  halt_ = h;
  fault_addr_ = addr;
  return h;
}

Halt Cpu::ecall() {
  const u32 a0 = x[10], a1 = x[11], a2 = x[12];
  switch (x[17]) {
    case 64: {  // write(fd, buf, len): fd is ignored, everything goes to output
      if (!mem_.in_range(a1, a2)) return stop(Halt::LoadFault, a1);
      for (u32 k = 0; k < a2; ++k) {
        u32 byte = 0;
        mem_.load(a1 + k, 1, byte);
        output_.push_back(static_cast<char>(byte));
      }
      x[10] = a2;
      return Halt::Running;
    }
    case 93:  // exit(code)
      exit_code_ = s32(a0);
      return stop(Halt::Exit, pc);
    default:
      return stop(Halt::BadSyscall, pc);
  }
}

Halt Cpu::run(u64 max_steps) {
  for (u64 n = 0; n < max_steps && halt_ == Halt::Running; ++n) step();
  return halt_ == Halt::Running ? Halt::StepLimit : halt_;
}

Halt Cpu::step() {
  if (halt_ != Halt::Running) return halt_;
  if (pc & 3) return stop(Halt::MisalignedFetch, pc);
  u32 in = 0;
  if (!mem_.load(pc, 4, in)) return stop(Halt::FetchFault, pc);

  const u32 op = in & 0x7f, rd = in >> 7 & 31, f3 = in >> 12 & 7, rs1 = in >> 15 & 31, rs2 = in >> 20 & 31,
            f7 = in >> 25;
  const u32 a = x[rs1], b = x[rs2];
  u32 next = pc + 4, result = 0;
  bool write = false;

  switch (op) {
    case 0x37:  // LUI
      result = in & 0xfffff000u;
      write = true;
      break;
    case 0x17:  // AUIPC
      result = pc + (in & 0xfffff000u);
      write = true;
      break;
    case 0x6f: {  // JAL
      const u32 target = pc + imm_j(in);
      if (target & 3) return stop(Halt::MisalignedFetch, target);
      result = next;
      next = target;
      write = true;
      break;
    }
    case 0x67: {  // JALR
      if (f3 != 0) return stop(Halt::IllegalInstruction, pc);
      const u32 target = (a + imm_i(in)) & ~1u;
      if (target & 3) return stop(Halt::MisalignedFetch, target);
      result = next;
      next = target;
      write = true;
      break;
    }
    case 0x63: {  // BRANCH
      bool take = false;
      switch (f3) {
        case 0: take = a == b; break;
        case 1: take = a != b; break;
        case 4: take = s32(a) < s32(b); break;
        case 5: take = s32(a) >= s32(b); break;
        case 6: take = a < b; break;
        case 7: take = a >= b; break;
        default: return stop(Halt::IllegalInstruction, pc);
      }
      if (take) {
        const u32 target = pc + imm_b(in);
        if (target & 3) return stop(Halt::MisalignedFetch, target);
        next = target;
      }
      break;
    }
    case 0x03: {  // LOAD
      const u32 addr = a + imm_i(in);
      u32 n = 0;
      bool sign = false;
      switch (f3) {
        case 0: n = 1; sign = true; break;  // lb
        case 1: n = 2; sign = true; break;  // lh
        case 2: n = 4; break;               // lw
        case 4: n = 1; break;               // lbu
        case 5: n = 2; break;               // lhu
        default: return stop(Halt::IllegalInstruction, pc);
      }
      u32 v = 0;
      if (!mem_.load(addr, n, v)) return stop(Halt::LoadFault, addr);
      result = (sign && n < 4) ? sext(v, static_cast<int>(8 * n)) : v;
      write = true;
      break;
    }
    case 0x23: {  // STORE
      const u32 addr = a + imm_s(in);
      if (f3 > 2) return stop(Halt::IllegalInstruction, pc);
      if (!mem_.store(addr, 1u << f3, b)) return stop(Halt::StoreFault, addr);
      break;
    }
    case 0x13: {  // OP-IMM
      const u32 imm = imm_i(in);
      const u32 shamt = rs2;
      switch (f3) {
        case 0: result = a + imm; break;
        case 2: result = s32(a) < s32(imm); break;
        case 3: result = a < imm; break;
        case 4: result = a ^ imm; break;
        case 6: result = a | imm; break;
        case 7: result = a & imm; break;
        case 1:
          if (f7 != 0) return stop(Halt::IllegalInstruction, pc);
          result = a << shamt;
          break;
        case 5:
          if (f7 == 0x00) result = a >> shamt;
          else if (f7 == 0x20) result = static_cast<u32>(s32(a) >> shamt);
          else return stop(Halt::IllegalInstruction, pc);
          break;
      }
      write = true;
      break;
    }
    case 0x33: {  // OP
      if (f7 == 0x01) {  // M extension
        switch (f3) {
          case 0: result = a * b; break;
          case 1: result = static_cast<u32>(static_cast<u64>(i64{s32(a)} * i64{s32(b)}) >> 32); break;
          case 2: result = static_cast<u32>(static_cast<u64>(i64{s32(a)} * static_cast<i64>(b)) >> 32); break;
          case 3: result = static_cast<u32>((u64{a} * u64{b}) >> 32); break;
          case 4:
            if (b == 0) result = ~0u;
            else if (a == kIntMin && b == ~0u) result = a;
            else result = static_cast<u32>(s32(a) / s32(b));
            break;
          case 5: result = b == 0 ? ~0u : a / b; break;
          case 6:
            if (b == 0) result = a;
            else if (a == kIntMin && b == ~0u) result = 0;
            else result = static_cast<u32>(s32(a) % s32(b));
            break;
          case 7: result = b == 0 ? a : a % b; break;
        }
      } else if (f7 == 0x00) {
        switch (f3) {
          case 0: result = a + b; break;
          case 1: result = a << (b & 31); break;
          case 2: result = s32(a) < s32(b); break;
          case 3: result = a < b; break;
          case 4: result = a ^ b; break;
          case 5: result = a >> (b & 31); break;
          case 6: result = a | b; break;
          case 7: result = a & b; break;
        }
      } else if (f7 == 0x20 && f3 == 0) {
        result = a - b;
      } else if (f7 == 0x20 && f3 == 5) {
        result = static_cast<u32>(s32(a) >> (b & 31));
      } else {
        return stop(Halt::IllegalInstruction, pc);
      }
      write = true;
      break;
    }
    case 0x0f:  // FENCE: one hart, no caches to order
      break;
    case 0x73:  // SYSTEM
      if (in == 0x00000073u) {
        if (ecall() != Halt::Running) {
          ++retired_;
          return halt_;
        }
      } else if (in == 0x00100073u) {
        return stop(Halt::Breakpoint, pc);
      } else {
        return stop(Halt::IllegalInstruction, pc);
      }
      break;
    default:
      return stop(Halt::IllegalInstruction, pc);
  }

  if (write && rd != 0) x[rd] = result;
  pc = next;
  ++retired_;
  return halt_;
}

}  // namespace bedrock
