#include "bedrock/assembler.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "bedrock/encode.hpp"

namespace bedrock {

namespace {

using u32 = std::uint32_t;
using i64 = std::int64_t;

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$'; }
bool ident_char(char c) { return ident_start(c) || std::isdigit(static_cast<unsigned char>(c)); }

bool fits_signed(i64 v, int bits) { return v >= -(i64{1} << (bits - 1)) && v < (i64{1} << (bits - 1)); }
bool fits_word(i64 v) { return v >= -(i64{1} << 31) && v < (i64{1} << 32); }

u32 hi20(u32 v) { return ((v + 0x800u) >> 12) & 0xfffffu; }
std::int32_t lo12(u32 v) { return static_cast<std::int32_t>(((v & 0xfffu) ^ 0x800u)) - 0x800; }

std::optional<int> parse_reg(std::string_view s) {
  static const char* const abi[32] = {"zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
                                      "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
                                      "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
  s = trim(s);
  if (s == "fp") return 8;
  for (int k = 0; k < 32; ++k)
    if (s == abi[k]) return k;
  if (s.size() >= 2 && s[0] == 'x') {
    int n = 0;
    auto [p, ec] = std::from_chars(s.data() + 1, s.data() + s.size(), n);
    if (ec == std::errc() && p == s.data() + s.size() && n >= 0 && n < 32) return n;
  }
  return std::nullopt;
}

// Split operands on commas that are outside quotes.
std::vector<std::string> split_operands(std::string_view s) {
  std::vector<std::string> out;
  s = trim(s);
  if (s.empty()) return out;
  std::string cur;
  bool quoted = false;
  for (std::size_t k = 0; k < s.size(); ++k) {
    const char c = s[k];
    if (quoted && c == '\\' && k + 1 < s.size()) {
      cur += c;
      cur += s[++k];
      continue;
    }
    if (c == '"') quoted = !quoted;
    if (c == ',' && !quoted) {
      out.emplace_back(trim(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.emplace_back(trim(cur));
  return out;
}

std::string strip_comment(std::string_view line) {
  bool quoted = false;
  for (std::size_t k = 0; k < line.size(); ++k) {
    if (quoted && line[k] == '\\') {
      ++k;
      continue;
    }
    if (line[k] == '"') quoted = !quoted;
    if (line[k] == '#' && !quoted) return std::string(line.substr(0, k));
  }
  return std::string(line);
}

std::optional<char> unescape(char c) {
  switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return '\0';
    case '\\': return '\\';
    case '"': return '"';
    case '\'': return '\'';
    default: return std::nullopt;
  }
}

struct Stmt {
  const std::string* file = nullptr;
  int line = 0;
  std::vector<std::string> labels;
  std::string mnem;
  std::vector<std::string> ops;
  i64 fixed = -1;  // size decision made in pass 1 (li words, .space bytes, .align bytes)
};

enum class Sec { Text, Data, Abs };

struct Sym {
  Sec sec = Sec::Abs;
  i64 value = 0;
  bool known = true;
};

struct Val {
  i64 v = 0;
  bool known = true;
};

struct OpInfo {
  char fmt;  // R I H(shift) L(load) S(store) B U J(jal) j(jalr) Z(fixed word)
  u32 opc, f3, f7;
};

const std::map<std::string_view, OpInfo>& op_table() {
  static const std::map<std::string_view, OpInfo> t = {
      {"add", {'R', 0x33, 0, 0x00}},   {"sub", {'R', 0x33, 0, 0x20}},   {"sll", {'R', 0x33, 1, 0x00}},
      {"slt", {'R', 0x33, 2, 0x00}},   {"sltu", {'R', 0x33, 3, 0x00}},  {"xor", {'R', 0x33, 4, 0x00}},
      {"srl", {'R', 0x33, 5, 0x00}},   {"sra", {'R', 0x33, 5, 0x20}},   {"or", {'R', 0x33, 6, 0x00}},
      {"and", {'R', 0x33, 7, 0x00}},   {"mul", {'R', 0x33, 0, 0x01}},   {"mulh", {'R', 0x33, 1, 0x01}},
      {"mulhsu", {'R', 0x33, 2, 0x01}}, {"mulhu", {'R', 0x33, 3, 0x01}}, {"div", {'R', 0x33, 4, 0x01}},
      {"divu", {'R', 0x33, 5, 0x01}},  {"rem", {'R', 0x33, 6, 0x01}},   {"remu", {'R', 0x33, 7, 0x01}},
      {"addi", {'I', 0x13, 0, 0}},     {"slti", {'I', 0x13, 2, 0}},     {"sltiu", {'I', 0x13, 3, 0}},
      {"xori", {'I', 0x13, 4, 0}},     {"ori", {'I', 0x13, 6, 0}},      {"andi", {'I', 0x13, 7, 0}},
      {"slli", {'H', 0x13, 1, 0x00}},  {"srli", {'H', 0x13, 5, 0x00}},  {"srai", {'H', 0x13, 5, 0x20}},
      {"lb", {'L', 0x03, 0, 0}},       {"lh", {'L', 0x03, 1, 0}},       {"lw", {'L', 0x03, 2, 0}},
      {"lbu", {'L', 0x03, 4, 0}},      {"lhu", {'L', 0x03, 5, 0}},      {"sb", {'S', 0x23, 0, 0}},
      {"sh", {'S', 0x23, 1, 0}},       {"sw", {'S', 0x23, 2, 0}},       {"beq", {'B', 0x63, 0, 0}},
      {"bne", {'B', 0x63, 1, 0}},      {"blt", {'B', 0x63, 4, 0}},      {"bge", {'B', 0x63, 5, 0}},
      {"bltu", {'B', 0x63, 6, 0}},     {"bgeu", {'B', 0x63, 7, 0}},     {"lui", {'U', 0x37, 0, 0}},
      {"auipc", {'U', 0x17, 0, 0}},    {"jal", {'J', 0x6f, 0, 0}},      {"jalr", {'j', 0x67, 0, 0}},
      {"ecall", {'Z', 0x00000073, 0, 0}}, {"ebreak", {'Z', 0x00100073, 0, 0}}, {"fence", {'Z', 0x0ff0000f, 0, 0}},
  };
  return t;
}

class Assembler {
 public:
  explicit Assembler(const std::vector<Source>& sources) {
    for (const Source& src : sources) parse(src);
  }

  Program run() {
    pass(false);
    const auto text_size = static_cast<u32>(text_.size());
    data_base_ = (text_size + 15) & ~15u;
    pass(true);
    Program p;
    if (text_.size() != text_size) errors_.push_back({"", 0, "internal: text size changed between passes"});
    p.text_size = static_cast<u32>(text_.size());
    p.data_base = data_base_;
    p.image = text_;
    p.image.resize(data_base_, 0);
    p.image.insert(p.image.end(), data_.begin(), data_.end());
    for (const auto& [name, sym] : syms_)
      p.symbols[name] = static_cast<u32>(resolve(sym));
    if (auto it = p.symbols.find("_start"); it != p.symbols.end()) p.entry = it->second;
    p.errors = std::move(errors_);
    return p;
  }

 private:
  void parse(const Source& src) {
    std::istringstream in(src.text);
    std::string raw;
    int number = 0;
    while (std::getline(in, raw)) {
      ++number;
      const std::string line = strip_comment(raw);
      Stmt s;
      s.file = &src.name;
      s.line = number;
      std::string_view rest = trim(line);
      // Leading labels: ident ':'
      for (;;) {
        std::size_t k = 0;
        if (rest.empty() || !ident_start(rest[0])) break;
        while (k < rest.size() && ident_char(rest[k])) ++k;
        std::size_t c = k;
        while (c < rest.size() && rest[c] == ' ') ++c;
        if (c >= rest.size() || rest[c] != ':') break;
        s.labels.emplace_back(rest.substr(0, k));
        rest = trim(rest.substr(c + 1));
      }
      if (!rest.empty()) {
        std::size_t k = 0;
        while (k < rest.size() && !std::isspace(static_cast<unsigned char>(rest[k]))) ++k;
        for (char c : rest.substr(0, k)) s.mnem += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        s.ops = split_operands(rest.substr(k));
      }
      if (!s.labels.empty() || !s.mnem.empty()) stmts_.push_back(std::move(s));
    }
  }

  // ---- errors and output ------------------------------------------------

  void err(const std::string& msg) {
    if (final_) errors_.push_back({*st_->file, st_->line, msg});
  }

  std::vector<std::uint8_t>& buf() { return cur_ == Sec::Text ? text_ : data_; }
  i64 offset() { return static_cast<i64>(buf().size()); }

  void emit(i64 v, int bytes) {
    for (int k = 0; k < bytes; ++k) buf().push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(v) >> (8 * k)));
  }

  void word(u32 w) {
    if (cur_ != Sec::Text) err("instruction outside .text");
    if (offset() % 4 != 0) err("instruction not 4-byte aligned");
    emit(w, 4);
  }

  // ---- expressions --------------------------------------------------------

  i64 resolve(const Sym& s) const { return s.sec == Sec::Data ? data_base_ + s.value : s.value; }

  struct Parser {
    Assembler& A;
    std::string_view s;
    std::size_t i = 0;
    std::string error;
    bool known = true;

    void ws() {
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    bool eat(char c) {
      ws();
      if (i < s.size() && s[i] == c) {
        ++i;
        return true;
      }
      return false;
    }
    i64 sum() {
      i64 v = product();
      for (;;) {
        if (eat('+')) v += product();
        else if (eat('-')) v -= product();
        else return v;
      }
    }
    i64 product() {
      i64 v = factor();
      while (eat('*')) v *= factor();
      return v;
    }
    i64 factor() {
      ws();
      if (i >= s.size()) {
        error = "expected a value";
        return 0;
      }
      if (eat('-')) return -factor();
      if (eat('+')) return factor();
      if (eat('(')) {
        i64 v = sum();
        if (!eat(')')) error = "missing ')'";
        return v;
      }
      if (s[i] == '%') {
        std::size_t j = i + 1;
        while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
        const std::string_view fn = s.substr(i + 1, j - i - 1);
        i = j;
        if (!eat('(')) {
          error = "expected '(' after %" + std::string(fn);
          return 0;
        }
        const auto v = static_cast<u32>(sum());
        if (!eat(')')) error = "missing ')'";
        if (fn == "hi") return hi20(v);
        if (fn == "lo") return lo12(v);
        error = "unknown operator %" + std::string(fn);
        return 0;
      }
      if (s[i] == '\'') {
        if (i + 2 < s.size() && s[i + 1] != '\\' && s[i + 2] == '\'') {
          const char c = s[i + 1];
          i += 3;
          return static_cast<unsigned char>(c);
        }
        if (i + 3 < s.size() && s[i + 1] == '\\' && s[i + 3] == '\'') {
          if (auto c = unescape(s[i + 2])) {
            i += 4;
            return static_cast<unsigned char>(*c);
          }
        }
        error = "bad character literal";
        return 0;
      }
      if (std::isdigit(static_cast<unsigned char>(s[i]))) {
        int base = 10;
        if (s.substr(i, 2) == "0x" || s.substr(i, 2) == "0X") base = 16, i += 2;
        else if (s.substr(i, 2) == "0b" || s.substr(i, 2) == "0B") base = 2, i += 2;
        std::uint64_t v = 0;
        auto [p, ec] = std::from_chars(s.data() + i, s.data() + s.size(), v, base);
        if (ec != std::errc() || v > 0xffffffffull) {
          error = "bad number";
          return 0;
        }
        i = static_cast<std::size_t>(p - s.data());
        return static_cast<i64>(v);
      }
      if (ident_start(s[i])) {
        std::size_t j = i;
        while (j < s.size() && ident_char(s[j])) ++j;
        const std::string name(s.substr(i, j - i));
        i = j;
        auto it = A.syms_.find(name);
        if (it == A.syms_.end()) {
          if (A.final_) error = "undefined symbol '" + name + "'";
          known = false;
          return 0;
        }
        // Data addresses aren't final until pass 1 ends.
        if (!A.final_ && (it->second.sec == Sec::Data || !it->second.known)) known = false;
        return A.resolve(it->second);
      }
      error = "unexpected '" + std::string(1, s[i]) + "'";
      return 0;
    }
  };

  std::optional<Val> eval(std::string_view text) {
    Parser p{*this, trim(text), 0, {}, true};
    const i64 v = p.sum();
    p.ws();
    if (p.error.empty() && p.i != p.s.size()) p.error = "unexpected '" + std::string(p.s.substr(p.i)) + "'";
    if (!p.error.empty()) {
      err(p.error);
      return std::nullopt;
    }
    return Val{v, p.known};
  }

  // ---- operands -----------------------------------------------------------

  bool need(std::size_t n) {
    if (st_->ops.size() == n) return true;
    err("'" + st_->mnem + "' expects " + std::to_string(n) + " operand" + (n == 1 ? "" : "s") + ", got " +
        std::to_string(st_->ops.size()));
    return false;
  }

  std::optional<u32> reg(const std::string& s) {
    if (auto r = parse_reg(s)) return static_cast<u32>(*r);
    err("bad register '" + s + "'");
    return std::nullopt;
  }

  std::optional<i64> imm(const std::string& s, int bits, bool is_unsigned = false) {
    auto v = eval(s);
    if (!v) return std::nullopt;
    const bool ok = is_unsigned ? (v->v >= 0 && v->v < (i64{1} << bits)) : fits_signed(v->v, bits);
    if (!ok && final_) {
      err("immediate " + std::to_string(v->v) + " out of range for " + std::to_string(bits) + "-bit " +
          (is_unsigned ? "unsigned" : "signed") + " field");
      return std::nullopt;
    }
    return v->v;
  }

  // "off(reg)" with an optional (possibly parenthesised) offset expression.
  bool mem(const std::string& s, i64& off, u32& base) {
    const std::string_view t = trim(s);
    if (t.empty() || t.back() != ')') {
      err("expected offset(register), got '" + s + "'");
      return false;
    }
    int depth = 0;
    std::size_t open = std::string_view::npos;
    for (std::size_t k = t.size(); k-- > 0;) {
      if (t[k] == ')') ++depth;
      if (t[k] == '(' && --depth == 0) {
        open = k;
        break;
      }
    }
    if (open == std::string_view::npos) {
      err("unbalanced parentheses in '" + s + "'");
      return false;
    }
    auto r = reg(std::string(t.substr(open + 1, t.size() - open - 2)));
    if (!r) return false;
    base = *r;
    off = 0;
    if (const auto o = trim(t.substr(0, open)); !o.empty()) {
      auto v = imm(std::string(o), 12);
      if (!v) return false;
      off = *v;
    }
    return true;
  }

  std::optional<std::int32_t> pc_offset(const std::string& target, int bits) {
    auto v = eval(target);
    if (!v) return std::nullopt;
    const i64 off = v->v - offset();
    if (final_ && (off % 2 != 0 || !fits_signed(off, bits))) {
      err("target '" + target + "' out of range (" + std::to_string(off) + " bytes)");
      return std::nullopt;
    }
    return static_cast<std::int32_t>(off);
  }

  // ---- instructions -------------------------------------------------------

  void base(const std::string& m, const std::vector<std::string>& o) {
    const auto& table = op_table();
    auto it = table.find(m);
    if (it == table.end()) {
      err("unknown instruction '" + m + "'");
      return;
    }
    const OpInfo& op = it->second;
    auto argc = [&](std::size_t n) {
      if (o.size() == n) return true;
      err("'" + m + "' expects " + std::to_string(n) + " operands, got " + std::to_string(o.size()));
      return false;
    };
    switch (op.fmt) {
      case 'R': {
        if (!argc(3)) return;
        auto rd = reg(o[0]), rs1 = reg(o[1]), rs2 = reg(o[2]);
        if (rd && rs1 && rs2) word(enc::r(op.f7, *rs2, *rs1, op.f3, *rd, op.opc));
        else word(0);
        return;
      }
      case 'I': {
        if (!argc(3)) return;
        auto rd = reg(o[0]), rs1 = reg(o[1]);
        auto v = imm(o[2], 12);
        word(rd && rs1 && v ? enc::i(static_cast<std::int32_t>(*v), *rs1, op.f3, *rd, op.opc) : 0);
        return;
      }
      case 'H': {
        if (!argc(3)) return;
        auto rd = reg(o[0]), rs1 = reg(o[1]);
        auto v = imm(o[2], 5, true);
        word(rd && rs1 && v ? enc::i(static_cast<std::int32_t>(*v | op.f7 << 5), *rs1, op.f3, *rd, op.opc) : 0);
        return;
      }
      case 'L': {
        if (!argc(2)) return;
        auto rd = reg(o[0]);
        i64 off = 0;
        u32 rs1 = 0;
        const bool ok = mem(o[1], off, rs1);
        word(rd && ok ? enc::i(static_cast<std::int32_t>(off), rs1, op.f3, *rd, op.opc) : 0);
        return;
      }
      case 'S': {
        if (!argc(2)) return;
        auto rs2 = reg(o[0]);
        i64 off = 0;
        u32 rs1 = 0;
        const bool ok = mem(o[1], off, rs1);
        word(rs2 && ok ? enc::s(static_cast<std::int32_t>(off), *rs2, rs1, op.f3, op.opc) : 0);
        return;
      }
      case 'B': {
        if (!argc(3)) return;
        auto rs1 = reg(o[0]), rs2 = reg(o[1]);
        auto off = pc_offset(o[2], 13);
        word(rs1 && rs2 && off ? enc::b(*off, *rs2, *rs1, op.f3, op.opc) : 0);
        return;
      }
      case 'U': {
        if (!argc(2)) return;
        auto rd = reg(o[0]);
        auto v = eval(o[1]);
        if (v && final_ && (v->v < -(1 << 19) || v->v > 0xfffff)) {
          err("immediate " + std::to_string(v->v) + " out of range for 20-bit field");
          v.reset();
        }
        word(rd && v ? enc::u(static_cast<u32>(v->v), *rd, op.opc) : 0);
        return;
      }
      case 'J': {
        if (o.size() != 1 && o.size() != 2) {
          err("'jal' expects 1 or 2 operands");
          return;
        }
        auto rd = o.size() == 2 ? reg(o[0]) : std::optional<u32>(1);
        auto off = pc_offset(o.back(), 21);
        word(rd && off ? enc::j(*off, *rd, op.opc) : 0);
        return;
      }
      case 'j': {
        // jalr rs | jalr rd, off(rs) | jalr rd, rs | jalr rd, rs, off
        std::optional<u32> rd = 1, rs1;
        i64 off = 0;
        bool ok = true;
        if (o.size() == 1) {
          rs1 = reg(o[0]);
        } else if (o.size() == 2) {
          rd = reg(o[0]);
          if (o[1].find('(') != std::string::npos) {
            u32 r = 0;
            ok = mem(o[1], off, r);
            rs1 = r;
          } else {
            rs1 = reg(o[1]);
          }
        } else if (o.size() == 3) {
          rd = reg(o[0]);
          rs1 = reg(o[1]);
          auto v = imm(o[2], 12);
          ok = v.has_value();
          if (v) off = *v;
        } else {
          err("'jalr' expects 1 to 3 operands");
          return;
        }
        word(rd && rs1 && ok ? enc::i(static_cast<std::int32_t>(off), *rs1, 0, *rd, op.opc) : 0);
        return;
      }
      case 'Z':
        if (m != "fence" && !argc(0)) return;
        word(op.opc);
        return;
    }
  }

  void instruction() {
    const std::string& m = st_->mnem;
    const auto& o = st_->ops;
    using V = std::vector<std::string>;

    if (m == "li") {
      if (!need(2)) return;
      auto rd = reg(o[0]);
      auto v = eval(o[1]);
      if (!final_) st_->fixed = (v && v->known && fits_signed(v->v, 12)) ? 1 : 2;
      if (v && final_ && !fits_word(v->v)) {
        err("li value " + std::to_string(v->v) + " does not fit in 32 bits");
        v.reset();
      }
      const u32 x = v ? static_cast<u32>(v->v) : 0;
      const u32 r = rd.value_or(0);
      if (st_->fixed == 1) {
        word(enc::i(lo12(x), 0, 0, r, 0x13));
      } else {
        word(enc::u(hi20(x), r, 0x37));
        word(enc::i(lo12(x), r, 0, r, 0x13));
      }
      return;
    }
    if (m == "la") {
      if (!need(2)) return;
      auto rd = reg(o[0]);
      auto v = eval(o[1]);
      const u32 off = v ? static_cast<u32>(v->v - offset()) : 0;
      const u32 r = rd.value_or(0);
      word(enc::u(hi20(off), r, 0x17));
      word(enc::i(lo12(off), r, 0, r, 0x13));
      return;
    }

    struct Rewrite {
      std::size_t argc;
      const char* to;
      std::vector<int> order;  // indices into operands; -1 = zero, -2 = ra, -3 = "0", -4 = "1", -5 = "-1"
    };
    static const std::map<std::string_view, Rewrite> pseudo = {
        {"nop", {0, "addi", {-1, -1, -3}}},  {"mv", {2, "addi", {0, 1, -3}}},     {"not", {2, "xori", {0, 1, -5}}},
        {"neg", {2, "sub", {0, -1, 1}}},     {"seqz", {2, "sltiu", {0, 1, -4}}},  {"snez", {2, "sltu", {0, -1, 1}}},
        {"j", {1, "jal", {-1, 0}}},          {"jr", {1, "jalr", {-1, 0, -3}}},    {"ret", {0, "jalr", {-1, -2, -3}}},
        {"call", {1, "jal", {-2, 0}}},       {"beqz", {2, "beq", {0, -1, 1}}},    {"bnez", {2, "bne", {0, -1, 1}}},
        {"bltz", {2, "blt", {0, -1, 1}}},    {"bgez", {2, "bge", {0, -1, 1}}},    {"blez", {2, "bge", {-1, 0, 1}}},
        {"bgtz", {2, "blt", {-1, 0, 1}}},    {"bgt", {3, "blt", {1, 0, 2}}},      {"ble", {3, "bge", {1, 0, 2}}},
        {"bgtu", {3, "bltu", {1, 0, 2}}},    {"bleu", {3, "bgeu", {1, 0, 2}}},
    };
    if (auto it = pseudo.find(m); it != pseudo.end()) {
      if (!need(it->second.argc)) return;
      V args;
      for (int k : it->second.order) {
        switch (k) {
          case -1: args.emplace_back("zero"); break;
          case -2: args.emplace_back("ra"); break;
          case -3: args.emplace_back("0"); break;
          case -4: args.emplace_back("1"); break;
          case -5: args.emplace_back("-1"); break;
          default: args.push_back(o[static_cast<std::size_t>(k)]);
        }
      }
      base(it->second.to, args);
      return;
    }
    base(m, o);
  }

  // ---- directives ---------------------------------------------------------

  std::optional<std::string> string_literal(std::string_view s) {
    s = trim(s);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') {
      err("expected a quoted string");
      return std::nullopt;
    }
    std::string out;
    for (std::size_t k = 1; k + 1 < s.size(); ++k) {
      if (s[k] == '\\' && k + 2 < s.size()) {
        auto c = unescape(s[++k]);
        if (!c) {
          err(std::string("unknown escape '\\") + s[k] + "'");
          return std::nullopt;
        }
        out += *c;
      } else {
        out += s[k];
      }
    }
    return out;
  }

  // Sizes must be the same in both passes, so they're fixed in pass 1.
  i64 constant_size(const std::string& text) {
    if (!final_) {
      auto v = eval(text);
      st_->fixed = (v && v->known && v->v >= 0) ? v->v : -1;
    }
    if (st_->fixed < 0) err("'" + st_->mnem + "' needs a non-negative constant known at this point");
    return st_->fixed;
  }

  void directive() {
    const std::string& m = st_->mnem;
    const auto& o = st_->ops;
    if (m == ".text") {
      cur_ = Sec::Text;
    } else if (m == ".data" || m == ".rodata" || m == ".bss") {
      cur_ = Sec::Data;
    } else if (m == ".section") {
      cur_ = (!o.empty() && o[0].rfind(".text", 0) == 0) ? Sec::Text : Sec::Data;
    } else if (m == ".globl" || m == ".global" || m == ".type" || m == ".size" || m == ".option") {
      // accepted for compatibility; there is no linker
    } else if (m == ".word" || m == ".half" || m == ".byte") {
      const int bytes = m == ".word" ? 4 : m == ".half" ? 2 : 1;
      for (const auto& e : o) {
        auto v = eval(e);
        const i64 lo = bytes == 4 ? -(i64{1} << 31) : -(i64{1} << (8 * bytes - 1));
        const i64 hi = (i64{1} << (8 * bytes)) - 1;
        if (v && final_ && (v->v < lo || v->v > hi)) err("value " + std::to_string(v->v) + " does not fit in " + m);
        emit(v ? v->v : 0, bytes);
      }
    } else if (m == ".ascii" || m == ".asciz" || m == ".string") {
      for (const auto& e : o) {
        // An unparsable string still has a size; emit nothing in either pass.
        if (auto s = string_literal(e)) {
          for (char c : *s) emit(static_cast<unsigned char>(c), 1);
          if (m != ".ascii") emit(0, 1);
        }
      }
    } else if (m == ".space" || m == ".zero") {
      if (o.empty() || o.size() > 2) {
        err("'" + m + "' expects a size and an optional fill byte");
        return;
      }
      const i64 n = constant_size(o[0]);
      i64 fill = 0;
      if (o.size() == 2)
        if (auto v = eval(o[1])) fill = v->v;
      for (i64 k = 0; k < n; ++k) emit(fill, 1);
    } else if (m == ".align" || m == ".p2align" || m == ".balign") {
      if (!need(1)) return;
      const i64 n = constant_size(o[0]);
      if (n < 0) return;
      const i64 to = m == ".balign" ? n : (i64{1} << std::min<i64>(n, 16));
      if (to <= 0) return;
      while (offset() % to != 0) {
        if (cur_ == Sec::Text && offset() % 4 == 0 && to >= 4) emit(0x13, 4);  // nop
        else emit(0, 1);
      }
    } else if (m == ".equ" || m == ".set") {
      if (!need(2)) return;
      auto v = eval(o[1]);
      Sym& s = syms_[o[0]];
      s.sec = Sec::Abs;
      s.value = v ? v->v : 0;
      s.known = v && v->known;
    } else {
      err("unknown directive '" + m + "'");
    }
  }

  // ---- passes -------------------------------------------------------------

  void pass(bool final) {
    final_ = final;
    text_.clear();
    data_.clear();
    cur_ = Sec::Text;
    for (Stmt& s : stmts_) {
      st_ = &s;
      for (const std::string& label : s.labels) {
        if (!final) {
          if (syms_.count(label)) errors_.push_back({*s.file, s.line, "duplicate label '" + label + "'"});
          else syms_[label] = Sym{cur_, offset(), true};
        }
      }
      if (s.mnem.empty()) continue;
      if (s.mnem[0] == '.') directive();
      else instruction();
    }
  }

  std::vector<Stmt> stmts_;
  std::map<std::string, Sym> syms_;
  std::vector<std::uint8_t> text_, data_;
  std::vector<AsmError> errors_;
  Sec cur_ = Sec::Text;
  bool final_ = false;
  u32 data_base_ = 0;
  Stmt* st_ = nullptr;
};

}  // namespace

Program assemble(const std::vector<Source>& sources) { return Assembler(sources).run(); }

Program assemble(std::string_view text) { return assemble({Source{"<input>", std::string(text)}}); }

std::string format_errors(const Program& p) {
  std::string out;
  for (const AsmError& e : p.errors) out += e.file + ":" + std::to_string(e.line) + ": error: " + e.message + "\n";
  return out;
}

bool load(const Program& p, Memory& mem, Cpu& cpu) {
  if (!p.ok() || !mem.write_block(0, p.image)) return false;
  cpu.reset(p.entry);
  return true;
}

}  // namespace bedrock
