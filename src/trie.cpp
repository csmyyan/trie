#include "trie.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "common/exception.h"
#include "common/move_blocked.h"
#include "common/tracked_value.h"

namespace trie {

template <class T>
auto Trie::Get(std::string_view key) const -> const T * {
  // TODO(Task 1): 沿 key 查找节点并检查值类型；不存在或类型不匹配时返回 nullptr。
  throw NotImplementedException("Trie::Get is not implemented.");
}

template <class T>
auto Trie::Put(std::string_view key, std::shared_ptr<T> value) const -> Trie {
  // TODO(Task 1): 复制修改路径，共享其余分支；支持空键、覆盖值和保留子键。
  // value 已包装为 shared_ptr<T>；CAS 重试复用该值对象，每次构造独立的新节点。
  throw NotImplementedException("Trie::Put is not implemented.");
}

auto Trie::Remove(std::string_view key) const -> Trie {
  // TODO(Task 1): 删除目标值并剪枝；保留前缀、子键以及旧快照。
  throw NotImplementedException("Trie::Remove is not implemented.");
}

// for test
template auto Trie::Get<uint32_t>(std::string_view key) const -> const uint32_t *;
template auto Trie::Get<uint64_t>(std::string_view key) const -> const uint64_t *;
template auto Trie::Get<std::string>(std::string_view key) const -> const std::string *;

using Integer = std::unique_ptr<uint32_t>;
template auto Trie::Get<Integer>(std::string_view key) const -> const Integer *;
template auto Trie::Get<MoveBlocked>(std::string_view key) const -> const MoveBlocked *;
template auto Trie::Get<TrackedPtr>(std::string_view key) const -> const TrackedPtr *;

template auto Trie::Put<uint32_t>(std::string_view, std::shared_ptr<uint32_t>) const -> Trie;
template auto Trie::Put<uint64_t>(std::string_view, std::shared_ptr<uint64_t>) const -> Trie;
template auto Trie::Put<std::string>(std::string_view, std::shared_ptr<std::string>) const -> Trie;
template auto Trie::Put<Integer>(std::string_view, std::shared_ptr<Integer>) const -> Trie;
template auto Trie::Put<MoveBlocked>(std::string_view, std::shared_ptr<MoveBlocked>) const -> Trie;
template auto Trie::Put<TrackedPtr>(std::string_view, std::shared_ptr<TrackedPtr>) const -> Trie;

}  // namespace trie
