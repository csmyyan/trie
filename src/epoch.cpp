#include "epoch.h"

#include <exception>
#include <limits>
#include <utility>

#include "common/exception.h"

namespace trie {

auto EpochManager::Enter() -> Guard {
  // TODO(Task 3): 最外层 Pin 时登记当前线程和 epoch；嵌套进入只增加深度，不刷新 local_epoch。
  // 登记完成后返回 Guard，调用者随后才能加载共享根。
  throw NotImplementedException("EpochManager::Enter is not implemented.");
}

void EpochManager::Exit(std::thread::id thread_id) noexcept {
  // TODO(Task 3): 在锁内减少对应线程的嵌套深度，最后一个 Guard 退出时删除登记。
  // 此函数为 noexcept；完成实现后删除下面的占位语句。
  std::terminate();
}

void EpochManager::Retire(std::function<void()> reclaim) {
  // TODO(Task 3): 在锁内保存当前 epoch 与回调；不推进 epoch，不执行回调。
  throw NotImplementedException("EpochManager::Retire is not implemented.");
}

auto EpochManager::TryAdvanceEpoch() -> bool {
  // TODO(Task 3): 所有活跃线程的 local_epoch 都等于当前 epoch 时，推进一轮并返回 true。
  // 无活跃线程时允许推进；存在旧 epoch 读者或计数已到上限时返回 false。
  throw NotImplementedException("EpochManager::TryAdvanceEpoch is not implemented.");
}

auto EpochManager::TryReclaim() -> std::size_t {
  // TODO(Task 3): 尝试推进一次，再筛选 global_epoch - retired_epoch >= 2 的条目。
  // 推进失败也要检查已有安全条目；在锁内移出，锁外执行回调，返回本次回收数量。
  throw NotImplementedException("EpochManager::TryReclaim is not implemented.");
}

}  // namespace trie
