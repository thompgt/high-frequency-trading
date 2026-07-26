// Tiny interning table so the hot path carries a 4-byte SymbolId instead of a
// std::string. Nothing on the tick path ever touches a string; names are
// resolved only when printing or writing CSV.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "hft/types.hpp"

namespace hft {

class SymbolTable {
 public:
  SymbolId intern(const std::string& name) {
    auto it = index_.find(name);
    if (it != index_.end()) return it->second;
    const SymbolId id = static_cast<SymbolId>(names_.size());
    names_.push_back(name);
    index_.emplace(name, id);
    return id;
  }

  const std::string& name(SymbolId id) const {
    static const std::string kUnknown = "?";
    return id < names_.size() ? names_[id] : kUnknown;
  }

  std::size_t size() const { return names_.size(); }
  const std::vector<std::string>& names() const { return names_; }

 private:
  std::vector<std::string> names_;
  std::unordered_map<std::string, SymbolId> index_;
};

}  // namespace hft
