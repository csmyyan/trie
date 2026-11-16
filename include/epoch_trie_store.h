#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <string_view>
#include <utility>

#include "epoch.h"
#include "trie.h"

namespace trie {

// 经典 EBR 与 Trie 的集成示例：先 Pin 再读根，旧版本退休后等待两轮推进。
class EpochTrieStore {
 public:
  using Node = TrieNode;

  class ReadGuard {
   public:
    ReadGuard(const ReadGuard &) = delete;
    auto operator=(const ReadGuard &) -> ReadGuard & = delete;
    ReadGuard(ReadGuard &&other) noexcept
        : epoch_guard_(std::move(other.epoch_guard_)), version_(std::exchange(other.version_, nullptr)) {}
    auto operator=(ReadGuard &&) -> ReadGuard & = delete;

    template <class T>
    auto Get(std::string_view key) const & -> const T * {
      return version_ == nullptr ? nullptr : version_->Get<T>(key);
    }
    template <class T>
    auto Get(std::string_view key) const && -> const T * = delete;

    auto GetRoot() const & -> const Node * {
      return version_ == nullptr ? nullptr : version_->GetRoot().get();
    }
    auto GetRoot() const && -> const Node * = delete;

   private:
    friend class EpochTrieStore;
    ReadGuard(EpochManager::Guard guard, const Trie *version)
        : epoch_guard_(std::move(guard)), version_(version) {}
    EpochManager::Guard epoch_guard_;
    const Trie *version_;
  };

  EpochTrieStore() = default;
  EpochTrieStore(const EpochTrieStore &) = delete;
  auto operator=(const EpochTrieStore &) -> EpochTrieStore & = delete;
  ~EpochTrieStore() {
    if (epochs_.ActiveReaders() != 0) {
      std::terminate();
    }
    delete root_.load(std::memory_order_relaxed);
  }

  auto Read() -> ReadGuard {
    // 先登记读者，再加载共享根；ReadGuard 存活期间保护该版本。
    auto guard = epochs_.Enter();
    const Trie *version = root_.load(std::memory_order_acquire);
    return ReadGuard(std::move(guard), version);
  }

  template <class T>
  void Put(std::string_view key, T value) {
    std::lock_guard<std::mutex> lock(write_lock_);
    const Trie *current = root_.load(std::memory_order_acquire);
    Publish(current->Put<T>(key, std::move(value)));
  }

  void Remove(std::string_view key) {
    std::lock_guard<std::mutex> lock(write_lock_);
    const Trie *current = root_.load(std::memory_order_acquire);
    Publish(current->Remove(key));
  }

  // 一次最多推进一轮；停止所有操作后调用两次，可清理剩余退休版本。
  auto TryReclaim() -> std::size_t { return epochs_.TryReclaim(); }
  auto ActiveReaders() const -> std::size_t { return epochs_.ActiveReaders(); }
  auto PendingRetired() const -> std::size_t { return epochs_.PendingRetired(); }
  auto CurrentEpoch() const -> uint64_t { return epochs_.CurrentEpoch(); }

 private:
  void Publish(Trie updated) {
    const Trie *next = new Trie(std::move(updated));
    // 先发布新根，再登记旧版本退休；Retire 不推进 epoch。
    const Trie *old = root_.exchange(next, std::memory_order_acq_rel);
    epochs_.Retire([old] { delete old; });
  }

  EpochManager epochs_;
  std::mutex write_lock_;
  std::atomic<const Trie *> root_{new Trie()};
};

}  // namespace trie
