#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace trie {

// 通过析构次数验证生命周期，不持有额外的节点引用。
struct TrackedValue {
  TrackedValue(std::atomic<int> &destroyed, uint32_t value) : destroyed_(destroyed), value_(value) {}
  ~TrackedValue() { ++destroyed_; }
  std::atomic<int> &destroyed_;
  uint32_t value_;
};

using TrackedPtr = std::unique_ptr<TrackedValue>;

}  // namespace trie
