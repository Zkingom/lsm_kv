#include "kv/db.h"

namespace kv {

void DB::Put(const std::string& key, const std::string& value) { mem_.Put(key, value); }

void DB::Delete(const std::string& key) { mem_.Delete(key); }

std::optional<std::string> DB::Get(const std::string& key) const {
  std::string value;
  if (mem_.Get(key, &value) == LookupResult::kFound) return value;
  return std::nullopt;
}

}  // namespace kv
