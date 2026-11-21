// Flat little-endian physical memory starting at address 0. Accesses outside
// it fail (the CPU turns that into a fault) instead of wrapping.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bedrock {

class Memory {
 public:
  explicit Memory(std::uint32_t size) : bytes_(size, 0) {}

  std::uint32_t size() const { return static_cast<std::uint32_t>(bytes_.size()); }

  bool in_range(std::uint32_t addr, std::uint32_t n) const { return addr <= size() && n <= size() - addr; }

  bool load(std::uint32_t addr, std::uint32_t n, std::uint32_t& out) const {
    if (!in_range(addr, n)) return false;
    out = 0;
    for (std::uint32_t k = 0; k < n; ++k) out |= std::uint32_t{bytes_[addr + k]} << (8 * k);
    return true;
  }

  bool store(std::uint32_t addr, std::uint32_t n, std::uint32_t value) {
    if (!in_range(addr, n)) return false;
    for (std::uint32_t k = 0; k < n; ++k) bytes_[addr + k] = static_cast<std::uint8_t>(value >> (8 * k));
    return true;
  }

  bool write_block(std::uint32_t addr, std::span<const std::uint8_t> data) {
    if (!in_range(addr, static_cast<std::uint32_t>(data.size()))) return false;
    for (std::size_t k = 0; k < data.size(); ++k) bytes_[addr + k] = data[k];
    return true;
  }

 private:
  std::vector<std::uint8_t> bytes_;
};

}  // namespace bedrock
