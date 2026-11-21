// RV32 instruction encoders, one per format (RISC-V unprivileged spec, ch. 2).
// Immediates are passed as their full signed value; the encoder scatters the
// bits. Range checks are the caller's job (the assembler reports them).
#pragma once

#include <cstdint>

namespace bedrock::enc {

constexpr std::uint32_t r(std::uint32_t f7, std::uint32_t rs2, std::uint32_t rs1, std::uint32_t f3,
                          std::uint32_t rd, std::uint32_t op) {
  return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}

constexpr std::uint32_t i(std::int32_t imm, std::uint32_t rs1, std::uint32_t f3, std::uint32_t rd,
                          std::uint32_t op) {
  return (static_cast<std::uint32_t>(imm) & 0xfff) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}

constexpr std::uint32_t s(std::int32_t imm, std::uint32_t rs2, std::uint32_t rs1, std::uint32_t f3,
                          std::uint32_t op) {
  const auto u = static_cast<std::uint32_t>(imm);
  return (u >> 5 & 0x7f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (u & 0x1f) << 7 | op;
}

constexpr std::uint32_t b(std::int32_t imm, std::uint32_t rs2, std::uint32_t rs1, std::uint32_t f3,
                          std::uint32_t op) {
  const auto u = static_cast<std::uint32_t>(imm);
  return (u >> 12 & 1) << 31 | (u >> 5 & 0x3f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 |
         (u >> 1 & 0xf) << 8 | (u >> 11 & 1) << 7 | op;
}

constexpr std::uint32_t u(std::uint32_t imm20, std::uint32_t rd, std::uint32_t op) {
  return (imm20 & 0xfffff) << 12 | rd << 7 | op;
}

constexpr std::uint32_t j(std::int32_t imm, std::uint32_t rd, std::uint32_t op) {
  const auto v = static_cast<std::uint32_t>(imm);
  return (v >> 20 & 1) << 31 | (v >> 1 & 0x3ff) << 21 | (v >> 11 & 1) << 20 | (v >> 12 & 0xff) << 12 |
         rd << 7 | op;
}

}  // namespace bedrock::enc
