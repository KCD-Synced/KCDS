// Where the signatures of one game build matched, kept between starts of the game: the client
// resolves hundreds of them and each scan walks the whole module, which held up every join.
#ifndef KCD2S_BRIDGE_SIGNATURE_CACHE_HPP
#define KCD2S_BRIDGE_SIGNATURE_CACHE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kcd2s::bridge {

class SignatureCache {
 public:
  // `build` names the game's module and the client's: a saved cache of any other is not used,
  // since a client resolves in its own order and may hook what a signature would match.
  explicit SignatureCache(std::string build);

  // Takes what serialized() wrote. Anything malformed, or of another build, leaves it empty.
  void load(std::string_view saved);

  // Where a signature matched. The caller compares the bytes there before it trusts this.
  std::optional<std::size_t> find(std::string_view pattern) const;

  void remember(std::string_view pattern, std::size_t offset);

  std::string serialized() const;

  // Whether it holds something load() did not bring.
  bool changed() const {
    return changed_;
  }

  std::size_t size() const {
    return offsets_.size();
  }

 private:
  std::string build_;
  std::unordered_map<std::uint64_t, std::size_t> offsets_;
  bool changed_ = false;
};

}  // namespace kcd2s::bridge

#endif  // KCD2S_BRIDGE_SIGNATURE_CACHE_HPP
