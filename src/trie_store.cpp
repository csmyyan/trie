#include "trie_store.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "common/exception.h"
#include "common/move_blocked.h"
#include "common/tracked_value.h"

namespace trie {

template <class T>
auto TrieStore::Get(std::string_view key) -> std::optional<ValueGuard<T>> {
  // TODO(Task 2A): 在根锁内取得快照，解锁后查询，用 ValueGuard 保护成功读取的值。
  throw NotImplementedException("TrieStore::Get is not implemented.");
}

template <class T>
void TrieStore::Put(std::string_view key, T value) {
  // TODO(Task 2A): 用写锁串行化更新，在根锁外构造新 Trie，再持根锁发布。
  throw NotImplementedException("TrieStore::Put is not implemented.");
}

void TrieStore::Remove(std::string_view key) {
  // TODO(Task 2A): 按与 Put 一致的锁顺序取得快照、删除键并发布新版本。
  throw NotImplementedException("TrieStore::Remove is not implemented.");
}

template <class T>
auto CasTrieStore::Get(std::string_view key) -> std::optional<ValueGuard<T>> {
  // TODO(Task 2B): 原子取得根快照，在快照上查询并返回保护结果的 ValueGuard。
  throw NotImplementedException("CasTrieStore::Get is not implemented.");
}

template <class T>
void CasTrieStore::Put(std::string_view key, T value) {
  // TODO(Task 2B): 准备一次值对象，用 CAS 发布候选版本；冲突后基于最新快照重建并重试。
  // 不可复制值只能从输入中移动一次，重试时复用同一个共享值对象。
  throw NotImplementedException("CasTrieStore::Put is not implemented.");
}

void CasTrieStore::Remove(std::string_view key) {
  // TODO(Task 2B): 构造删除结果并用 CAS 发布；失败后基于最新快照重新删除。
  throw NotImplementedException("CasTrieStore::Remove is not implemented.");
}

template auto TrieStore::Get(std::string_view key) -> std::optional<ValueGuard<uint32_t>>;
template void TrieStore::Put(std::string_view key, uint32_t value);
template auto TrieStore::Get(std::string_view key) -> std::optional<ValueGuard<std::string>>;
template void TrieStore::Put(std::string_view key, std::string value);

using Integer = std::unique_ptr<uint32_t>;
template auto TrieStore::Get(std::string_view key) -> std::optional<ValueGuard<Integer>>;
template void TrieStore::Put(std::string_view key, Integer value);
template auto TrieStore::Get(std::string_view key) -> std::optional<ValueGuard<MoveBlocked>>;
template void TrieStore::Put(std::string_view key, MoveBlocked value);
template auto TrieStore::Get(std::string_view key) -> std::optional<ValueGuard<TrackedPtr>>;
template void TrieStore::Put(std::string_view key, TrackedPtr value);

template auto CasTrieStore::Get(std::string_view) -> std::optional<ValueGuard<uint32_t>>;
template void CasTrieStore::Put(std::string_view, uint32_t);
template auto CasTrieStore::Get(std::string_view) -> std::optional<ValueGuard<std::string>>;
template void CasTrieStore::Put(std::string_view, std::string);
template auto CasTrieStore::Get(std::string_view) -> std::optional<ValueGuard<Integer>>;
template void CasTrieStore::Put(std::string_view, Integer);
template auto CasTrieStore::Get(std::string_view) -> std::optional<ValueGuard<MoveBlocked>>;
template void CasTrieStore::Put(std::string_view, MoveBlocked);
template auto CasTrieStore::Get(std::string_view) -> std::optional<ValueGuard<TrackedPtr>>;
template void CasTrieStore::Put(std::string_view, TrackedPtr);

}  // namespace trie
