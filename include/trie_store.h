#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "trie.h"

namespace trie {

template <class T>
class ValueGuard {
 public:
  ValueGuard(Trie root, const T &value) : root_(std::move(root)), value_(value) {}
  auto operator*() const -> const T & { return value_; }

 private:
  Trie root_;
  const T &value_;
};

// Task 2A：使用两把互斥锁保护根快照和完整写操作；写者串行，读者在快照上并行查询。
class TrieStore {
 public:
  template <class T>
  auto Get(std::string_view key) -> std::optional<ValueGuard<T>>;

  template <class T>
  void Put(std::string_view key, T value);

  void Remove(std::string_view key);

 private:
  std::mutex root_lock_;
  std::mutex write_lock_;
  Trie root_;
};

// Task 2B：多个写者可同时构造候选版本，通过 CAS 检查旧快照是否仍有效。
class CasTrieStore {
 public:
  CasTrieStore() = default;
  CasTrieStore(const CasTrieStore &) = delete;
  auto operator=(const CasTrieStore &) -> CasTrieStore & = delete;

  template <class T>
  auto Get(std::string_view key) -> std::optional<ValueGuard<T>>;

  template <class T>
  void Put(std::string_view key, T value);

  void Remove(std::string_view key);

 private:
  std::shared_ptr<const TrieNode> root_;
};

}  // namespace trie
