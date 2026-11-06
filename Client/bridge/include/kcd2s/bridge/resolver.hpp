// Resolver — a keyed table of signatures that locates engine functions and globals in a module
// image at load time. A spec either resolves to a function entry (the match
// offset) or, for a global referenced RIP-relative (e.g. gEnv), to the global's offset via the
// displacement inside the matched instruction. Pure logic over a byte span -> headless.
#ifndef KCD2S_BRIDGE_RESOLVER_HPP
#define KCD2S_BRIDGE_RESOLVER_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "kcd2s/bridge/sigscan.hpp"

namespace kcd2s::bridge {

struct ResolveSpec {
  Signature sig;
  // disp_at >= 0: the match holds a 4-byte RIP-relative displacement at match+disp_at, instruction
  // ending at match+insn_len; resolve() returns the referenced global's image offset. Otherwise
  // resolve() returns the match offset itself (a function entry point).
  int disp_at = -1;
  int insn_len = 0;
};

class Resolver {
 public:
  // Register a key -> pattern. Returns false if the pattern is malformed.
  bool add(const std::string& key, std::string_view pattern, int disp_at = -1, int insn_len = 0) {
    auto sig = Signature::parse(pattern);
    if (!sig)
      return false;
    specs_[key] = ResolveSpec{std::move(*sig), disp_at, insn_len};
    return true;
  }

  // Register a pre-parsed signature.
  void add_signature(const std::string& key, Signature sig, int disp_at = -1, int insn_len = 0) {
    specs_[key] = ResolveSpec{std::move(sig), disp_at, insn_len};
  }

  bool has(const std::string& key) const {
    return specs_.count(key) != 0;
  }
  std::size_t size() const {
    return specs_.size();
  }

  // Resolve a key against the module image. Returns the image-relative offset of the function entry
  // (or the referenced global), or nullopt if the key is unknown or the pattern is not found.
  std::optional<std::size_t> resolve(const std::string& key, const std::uint8_t* image,
                                     std::size_t len) const {
    auto it = specs_.find(key);
    if (it == specs_.end())
      return std::nullopt;
    const ResolveSpec& spec = it->second;
    const std::size_t off = scan(image, len, spec.sig);
    if (off == knpos)
      return std::nullopt;
    if (spec.disp_at < 0)
      return off;  // function entry
    const std::size_t tgt = rip_target(image, len, off + static_cast<std::size_t>(spec.disp_at),
                                       off + static_cast<std::size_t>(spec.insn_len));
    if (tgt == knpos)
      return std::nullopt;
    return tgt;
  }

 private:
  std::unordered_map<std::string, ResolveSpec> specs_;
};

}  // namespace kcd2s::bridge

#endif  // KCD2S_BRIDGE_RESOLVER_HPP
