#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace trie {

// 实验三：经典 EBR 的推进与两轮回收协议；用短时互斥锁同步元数据。
class EpochManager {
 public:
  class Guard {
   public:
    Guard(const Guard &) = delete;
    auto operator=(const Guard &) -> Guard & = delete;
    Guard(Guard &&other) noexcept
        : manager_(std::exchange(other.manager_, nullptr)), thread_id_(other.thread_id_) {}

    auto operator=(Guard &&other) noexcept -> Guard & {
      if (this != &other) {
        if (manager_ != nullptr) {
          manager_->Exit(thread_id_);
        }
        manager_ = std::exchange(other.manager_, nullptr);
        thread_id_ = other.thread_id_;
      }
      return *this;
    }

    ~Guard() {
      if (manager_ != nullptr) {
        manager_->Exit(thread_id_);
      }
    }

   private:
    friend class EpochManager;
    Guard(EpochManager *manager, std::thread::id thread_id) : manager_(manager), thread_id_(thread_id) {}
    EpochManager *manager_;
    std::thread::id thread_id_;
  };

  EpochManager() = default;
  EpochManager(const EpochManager &) = delete;
  auto operator=(const EpochManager &) -> EpochManager & = delete;

  ~EpochManager() {
    // 所有操作和 Guard 均已结束，退出时可直接清理剩余退休对象。
    if (!participants_.empty()) {
      std::terminate();
    }
    auto pending = std::move(retired_);
    for (auto &entry : pending) {
      entry.reclaim();
    }
  }

  // Pin：最外层进入时记录当前 epoch；嵌套进入只增加深度，不刷新 local_epoch。
  // 完成登记后才能加载共享根。
  auto Enter() -> Guard;

  // 对象从共享结构移除后记录退休 epoch 和回调；不推进 epoch，也不执行回调。
  void Retire(std::function<void()> reclaim);

  // 所有活跃线程的 local_epoch 都等于当前 epoch 时，推进一轮并返回 true。
  // 无活跃线程时也允许推进；一次调用最多推进一轮，不执行回调。
  auto TryAdvanceEpoch() -> bool;

  // 尝试推进一轮，再回收已经经历至少两轮推进的条目。
  // 即使本轮无法推进，也要检查已有的安全条目；回调在锁外执行。
  auto TryReclaim() -> std::size_t;

  auto ActiveReaders() const -> std::size_t {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t readers = 0;
    for (const auto &entry : participants_) {
      readers += entry.second.depth;
    }
    return readers;
  }

  auto PendingRetired() const -> std::size_t {
    std::lock_guard<std::mutex> lock(mutex_);
    return retired_.size();
  }

  auto CurrentEpoch() const -> uint64_t {
    std::lock_guard<std::mutex> lock(mutex_);
    return epoch_;
  }

 private:
  // Unpin：减少进入线程的嵌套深度；最后一个 Guard 退出时删除登记。
  void Exit(std::thread::id thread_id) noexcept;

  struct Participant {
    uint64_t local_epoch;
    std::size_t depth;
  };

  struct Retired {
    uint64_t epoch;
    std::function<void()> reclaim;
  };

  mutable std::mutex mutex_;
  uint64_t epoch_{0};
  // 不在表中的线程视为 inactive；每个活跃线程只有一份登记。
  std::unordered_map<std::thread::id, Participant> participants_;
  std::vector<Retired> retired_;
};

}  // namespace trie
