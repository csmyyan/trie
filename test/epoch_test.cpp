#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/tracked_value.h"
#include "epoch.h"
#include "epoch_trie_store.h"
#include "test.h"
#include "test_utils.h"

namespace trie {

// 用于操作已经停止后的最终清理；单轮行为由下方独立用例验证。
template <class Reclaimer>
auto ReclaimTwoRounds(Reclaimer &reclaimer) -> std::size_t {
  auto reclaimed = reclaimer.TryReclaim();
  return reclaimed + reclaimer.TryReclaim();
}

TEST(StarterSupport, EmptyEpochState) {
  EpochManager epochs;
  ASSERT_EQ(epochs.CurrentEpoch(), 0U);
  ASSERT_EQ(epochs.ActiveReaders(), 0U);
  ASSERT_EQ(epochs.PendingRetired(), 0U);
  EpochTrieStore store;
  ASSERT_EQ(store.CurrentEpoch(), 0U);
  ASSERT_EQ(store.ActiveReaders(), 0U);
  ASSERT_EQ(store.PendingRetired(), 0U);
}

TEST(EpochTest, NoReadersNeedTwoEpochsAndExactlyOnce) {
  int destroyed = 0;
  EpochManager epochs;
  epochs.Retire([&] { ++destroyed; });
  ASSERT_EQ(epochs.CurrentEpoch(), 0U);
  ASSERT_EQ(epochs.PendingRetired(), 1U);

  ASSERT_EQ(epochs.TryReclaim(), 0U);
  ASSERT_EQ(epochs.CurrentEpoch(), 1U);
  ASSERT_EQ(destroyed, 0);
  ASSERT_EQ(epochs.TryReclaim(), 1U);
  ASSERT_EQ(epochs.CurrentEpoch(), 2U);
  ASSERT_EQ(destroyed, 1);
  ASSERT_EQ(epochs.TryReclaim(), 0U);
  ASSERT_EQ(destroyed, 1);
}

TEST(EpochTest, RetireDoesNotAdvanceEpoch) {
  int destroyed = 0;
  EpochManager epochs;
  for (int i = 0; i < 3; ++i) {
    epochs.Retire([&] { ++destroyed; });
    ASSERT_EQ(epochs.CurrentEpoch(), 0U);
  }
  ASSERT_TRUE(epochs.TryAdvanceEpoch());
  ASSERT_EQ(epochs.CurrentEpoch(), 1U);
  ASSERT_TRUE(epochs.TryAdvanceEpoch());
  ASSERT_EQ(epochs.CurrentEpoch(), 2U);
  ASSERT_EQ(destroyed, 0);  // 推进代际本身不执行回调。
  ASSERT_EQ(epochs.TryReclaim(), 3U);
  ASSERT_EQ(destroyed, 3);
}

TEST(EpochTest, SameEpochReaderBlocksSecondAdvance) {
  int destroyed = 0;
  EpochManager epochs;
  {
    auto guard = epochs.Enter();
    epochs.Retire([&] { ++destroyed; });
    ASSERT_EQ(epochs.TryReclaim(), 0U);
    ASSERT_EQ(epochs.CurrentEpoch(), 1U);
    ASSERT_EQ(epochs.TryReclaim(), 0U);
    ASSERT_EQ(epochs.CurrentEpoch(), 1U);
    ASSERT_EQ(destroyed, 0);
  }
  ASSERT_EQ(epochs.ActiveReaders(), 0U);
  ASSERT_EQ(epochs.TryReclaim(), 1U);
  ASSERT_EQ(epochs.CurrentEpoch(), 2U);
  ASSERT_EQ(destroyed, 1);
}

TEST(EpochTest, LaterReaderDoesNotBlockOlderRetirement) {
  int destroyed = 0;
  EpochManager epochs;
  epochs.Retire([&] { ++destroyed; });  // E0
  ASSERT_TRUE(epochs.TryAdvanceEpoch());  // E1
  auto late = epochs.Enter();            // local_epoch = E1
  ASSERT_TRUE(epochs.TryAdvanceEpoch());  // E2
  ASSERT_TRUE(!epochs.TryAdvanceEpoch());

  // late 阻止 E2 -> E3，但 E0 的对象已经安全，仍必须回收。
  ASSERT_EQ(epochs.TryReclaim(), 1U);
  ASSERT_EQ(epochs.CurrentEpoch(), 2U);
  ASSERT_EQ(epochs.ActiveReaders(), 1U);
  ASSERT_EQ(destroyed, 1);
}

TEST(EpochTest, PartialReclamationKeepsNewerRetirements) {
  int old_destroyed = 0;
  int new_destroyed = 0;
  EpochManager epochs;
  epochs.Retire([&] { ++old_destroyed; });  // E0
  ASSERT_EQ(epochs.TryReclaim(), 0U);        // E1
  epochs.Retire([&] { ++new_destroyed; });  // E1
  {
    auto reader = epochs.Enter();
    ASSERT_EQ(epochs.TryReclaim(), 1U);     // E2，只回收 E0。
    ASSERT_EQ(old_destroyed, 1);
    ASSERT_EQ(new_destroyed, 0);
    ASSERT_EQ(epochs.PendingRetired(), 1U);
    ASSERT_EQ(epochs.TryReclaim(), 0U);     // E1 读者阻止进入 E3。
    ASSERT_EQ(epochs.CurrentEpoch(), 2U);
  }
  ASSERT_EQ(epochs.TryReclaim(), 1U);       // E3，可以回收 E1。
  ASSERT_EQ(old_destroyed, 1);
  ASSERT_EQ(new_destroyed, 1);
  ASSERT_EQ(epochs.PendingRetired(), 0U);
}

TEST(EpochTest, LongReaderBlocksLaterEpochs) {
  int destroyed = 0;
  EpochManager epochs;
  {
    auto old_reader = epochs.Enter();
    epochs.Retire([&] { ++destroyed; });  // E0
    ASSERT_EQ(epochs.TryReclaim(), 0U);   // E1
    for (int i = 0; i < 8; ++i) {
      epochs.Retire([&] { ++destroyed; });  // 都退休于 E1。
      ASSERT_EQ(epochs.TryReclaim(), 0U);
      ASSERT_EQ(epochs.CurrentEpoch(), 1U);
    }
    ASSERT_EQ(destroyed, 0);
    ASSERT_EQ(epochs.PendingRetired(), 9U);
  }
  ASSERT_EQ(epochs.TryReclaim(), 1U);     // E2，只回收 E0。
  ASSERT_EQ(epochs.TryReclaim(), 8U);     // E3，回收 E1。
  ASSERT_EQ(destroyed, 9);
}

TEST(EpochTest, NestedGuardsKeepOldEpoch) {
  EpochManager epochs;
  std::optional<EpochManager::Guard> outer(epochs.Enter());  // E0
  ASSERT_TRUE(epochs.TryAdvanceEpoch());                    // E1
  {
    auto inner = epochs.Enter();  // 同线程嵌套，必须保留 E0。
    ASSERT_EQ(epochs.ActiveReaders(), 2U);
    ASSERT_TRUE(!epochs.TryAdvanceEpoch());
    outer.reset();  // Guard 可乱序销毁，只在深度归零时 Unpin。
    ASSERT_EQ(epochs.ActiveReaders(), 1U);
    ASSERT_TRUE(!epochs.TryAdvanceEpoch());
    ASSERT_EQ(epochs.CurrentEpoch(), 1U);
  }
  ASSERT_EQ(epochs.ActiveReaders(), 0U);
  auto next = epochs.Enter();  // 最外层重新进入，登记 E1。
  ASSERT_TRUE(epochs.TryAdvanceEpoch());
  ASSERT_EQ(epochs.CurrentEpoch(), 2U);
}

TEST(EpochTest, MovedGuardsTransferRegistration) {
  int destroyed = 0;
  EpochManager epochs;
  {
    auto original = epochs.Enter();
    auto moved = std::move(original);
    ASSERT_EQ(epochs.ActiveReaders(), 1U);
    auto other = epochs.Enter();
    ASSERT_EQ(epochs.ActiveReaders(), 2U);
    moved = std::move(other);
    ASSERT_EQ(epochs.ActiveReaders(), 1U);
    epochs.Retire([&] { ++destroyed; });
    ASSERT_EQ(epochs.TryReclaim(), 0U);
  }
  ASSERT_EQ(epochs.ActiveReaders(), 0U);
  ASSERT_EQ(epochs.TryReclaim(), 1U);
  ASSERT_EQ(destroyed, 1);
}

TEST(EpochTest, AllActiveThreadsMustReachCurrentEpoch) {
  int destroyed = 0;
  EpochManager epochs;
  std::optional<EpochManager::Guard> main_reader(epochs.Enter());
  std::promise<void> entered;
  auto ready = entered.get_future();
  std::promise<void> release;
  auto may_exit = release.get_future();
  auto worker = std::async(std::launch::async, [&] {
    std::optional<EpochManager::Guard> guard;
    try {
      guard.emplace(epochs.Enter());
    } catch (...) {
      entered.set_exception(std::current_exception());
      throw;
    }
    entered.set_value();
    may_exit.wait();
  });

  try {
    ready.get();
    epochs.Retire([&] { ++destroyed; });
    ASSERT_EQ(epochs.ActiveReaders(), 2U);
    ASSERT_TRUE(epochs.TryAdvanceEpoch());
    main_reader.reset();
    ASSERT_EQ(epochs.ActiveReaders(), 1U);
    ASSERT_TRUE(!epochs.TryAdvanceEpoch());  // 另一个线程仍停留在 E0。
    ASSERT_EQ(epochs.CurrentEpoch(), 1U);
    ASSERT_EQ(destroyed, 0);
  } catch (...) {
    release.set_value();
    worker.wait();
    throw;
  }
  release.set_value();
  worker.get();
  ASSERT_EQ(epochs.TryReclaim(), 1U);
  ASSERT_EQ(destroyed, 1);
}

TEST(EpochTest, CallbackRunsOutsideMetadataLock) {
  std::size_t pending_seen = 99;
  int followup_destroyed = 0;
  EpochManager epochs;
  epochs.Retire([&] {
    pending_seen = epochs.PendingRetired();
    epochs.Retire([&] { ++followup_destroyed; });
  });
  ASSERT_EQ(epochs.TryReclaim(), 0U);
  ASSERT_EQ(epochs.TryReclaim(), 1U);
  ASSERT_EQ(pending_seen, 0U);
  ASSERT_EQ(epochs.PendingRetired(), 1U);
  ASSERT_EQ(followup_destroyed, 0);
  ASSERT_EQ(epochs.TryReclaim(), 0U);
  ASSERT_EQ(epochs.TryReclaim(), 1U);
  ASSERT_EQ(followup_destroyed, 1);
}

TEST(EpochTest, ConcurrentReadersAndRetirement) {
  std::atomic<int> destroyed{0};
  std::atomic<int> premature{0};
  std::array<std::atomic<bool>, 2000> reading_flags{};
  EpochManager epochs;
  std::vector<std::future<void>> workers;
  for (int thread = 0; thread < 4; ++thread) {
    workers.push_back(std::async(std::launch::async, [&, thread] {
      for (int i = 0; i < 500; ++i) {
        auto guard = epochs.Enter();
        auto *reading = &reading_flags[thread * 500 + i];
        reading->store(true);
        epochs.Retire([reading, &destroyed, &premature] {
          if (reading->load()) {
            ++premature;
          }
          ++destroyed;
        });
        epochs.TryReclaim();
        reading->store(false);
      }
    }));
  }
  for (auto &worker : workers) {
    worker.get();
  }
  ReclaimTwoRounds(epochs);
  ASSERT_EQ(premature.load(), 0);
  ASSERT_EQ(destroyed.load(), 2000);
  ASSERT_EQ(epochs.ActiveReaders(), 0U);
  ASSERT_EQ(epochs.PendingRetired(), 0U);
}

TEST(EpochTest, ConcurrentCollectorsReclaimExactlyOnce) {
  std::array<std::atomic<int>, 256> calls{};
  EpochManager epochs;
  for (auto &count : calls) {
    epochs.Retire([counter = &count] { ++*counter; });
  }
  std::vector<std::future<void>> workers;
  for (int thread = 0; thread < 4; ++thread) {
    workers.push_back(std::async(std::launch::async, [&] {
      for (int i = 0; i < 50; ++i) {
        epochs.TryReclaim();
      }
    }));
  }
  for (auto &worker : workers) {
    worker.get();
  }
  ReclaimTwoRounds(epochs);
  for (const auto &count : calls) {
    ASSERT_EQ(count.load(), 1);
  }
  ASSERT_EQ(epochs.PendingRetired(), 0U);
}

TEST(EpochTest, DestructorDrainsRetiredObjects) {
  int destroyed = 0;
  {
    EpochManager epochs;
    epochs.Retire([&] { ++destroyed; });
    ASSERT_EQ(epochs.CurrentEpoch(), 0U);
    ASSERT_EQ(destroyed, 0);
  }
  ASSERT_EQ(destroyed, 1);
}

TEST(EpochTest, EmptyQueueAdvancesAtMostOncePerCall) {
  EpochManager epochs;
  ASSERT_TRUE(epochs.TryAdvanceEpoch());
  ASSERT_EQ(epochs.CurrentEpoch(), 1U);
  for (uint64_t expected = 2; expected <= 4; ++expected) {
    ASSERT_EQ(epochs.TryReclaim(), 0U);
    ASSERT_EQ(epochs.CurrentEpoch(), expected);
    ASSERT_EQ(epochs.PendingRetired(), 0U);
  }
  {
    auto reader = epochs.Enter();  // E4
    ASSERT_EQ(epochs.TryReclaim(), 0U);
    ASSERT_EQ(epochs.CurrentEpoch(), 5U);
    for (int repeat = 0; repeat < 3; ++repeat) {
      ASSERT_EQ(epochs.TryReclaim(), 0U);
      ASSERT_TRUE(!epochs.TryAdvanceEpoch());
      ASSERT_EQ(epochs.CurrentEpoch(), 5U);
      ASSERT_EQ(epochs.ActiveReaders(), 1U);
    }
  }
  ASSERT_EQ(epochs.TryReclaim(), 0U);
  ASSERT_EQ(epochs.CurrentEpoch(), 6U);
  ASSERT_EQ(epochs.ActiveReaders(), 0U);
  ASSERT_EQ(epochs.PendingRetired(), 0U);
}

TEST(EpochTest, TransferredGuardUnpinsItsOriginThread) {
  std::cout << "  scenario: a worker releases the main thread's Guard while keeping its own Pin" << std::endl;
  std::atomic<int> reclaimed{0};
  std::promise<void> entered;
  auto ready = entered.get_future();
  std::promise<void> release;
  auto may_continue = release.get_future();
  EpochManager epochs;
  auto original = epochs.Enter();  // Main thread, E0.
  auto worker = std::async(std::launch::async, [&, transferred = std::move(original)]() mutable {
    // Explicitly destroy the owner in this thread; an async callable's capture can outlive its invocation.
    std::optional<EpochManager::Guard> owner(std::move(transferred));
    entered.set_value();
    may_continue.wait();
    auto local = epochs.Enter();  // Worker thread, E1.
    ASSERT_EQ(epochs.ActiveReaders(), 2U);
    ASSERT_TRUE(!epochs.TryAdvanceEpoch());
    owner.reset();  // Must remove the main thread's registration, not the worker's.
    ASSERT_EQ(epochs.ActiveReaders(), 1U);
    ASSERT_EQ(epochs.TryReclaim(), 1U);
    ASSERT_EQ(epochs.CurrentEpoch(), 2U);
    ASSERT_EQ(reclaimed.load(), 1);
  });
  try {
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ready.get();
    ASSERT_EQ(epochs.ActiveReaders(), 1U);
    epochs.Retire([&] { ++reclaimed; });
    ASSERT_EQ(epochs.TryReclaim(), 0U);
    ASSERT_EQ(epochs.CurrentEpoch(), 1U);
    ASSERT_EQ(reclaimed.load(), 0);
  } catch (...) {
    release.set_value();
    worker.wait();
    throw;
  }
  release.set_value();
  worker.get();
  ASSERT_EQ(epochs.ActiveReaders(), 0U);
  ASSERT_EQ(epochs.PendingRetired(), 0U);
  ASSERT_EQ(reclaimed.load(), 1);
}

TEST(EpochTest, MoveAssignmentBetweenManagersKeepsRegistrationsSeparate) {
  int first_reclaimed = 0;
  int second_reclaimed = 0;
  EpochManager first;
  EpochManager second;
  auto destination = first.Enter();
  auto source = second.Enter();
  first.Retire([&] { ++first_reclaimed; });
  second.Retire([&] { ++second_reclaimed; });

  destination = std::move(source);
  ASSERT_EQ(first.ActiveReaders(), 0U);
  ASSERT_EQ(second.ActiveReaders(), 1U);
  ASSERT_EQ(ReclaimTwoRounds(first), 1U);
  ASSERT_EQ(first_reclaimed, 1);
  ASSERT_EQ(second.CurrentEpoch(), 0U);
  ASSERT_EQ(second.TryReclaim(), 0U);  // E1, still protected by destination.

  auto &alias = destination;
  destination = std::move(alias);  // Self-move must not accidentally Unpin.
  ASSERT_EQ(second.ActiveReaders(), 1U);
  ASSERT_EQ(second.TryReclaim(), 0U);
  ASSERT_EQ(second_reclaimed, 0);

  destination = std::move(source);  // The moved-from source is empty: release destination's Pin.
  ASSERT_EQ(second.ActiveReaders(), 0U);
  ASSERT_EQ(second.TryReclaim(), 1U);
  ASSERT_EQ(second_reclaimed, 1);
  ASSERT_EQ(first.PendingRetired(), 0U);
  ASSERT_EQ(second.PendingRetired(), 0U);
}

TEST(EpochTest, CallbackCanReenterReclaimWithoutRepeatingItself) {
  std::cout << "  scenario: a callback retires another object and reenters TryReclaim" << std::endl;
  int first_reclaimed = 0;
  int second_reclaimed = 0;
  std::size_t nested_result = 99;
  EpochManager epochs;
  epochs.Retire([&] {
    ++first_reclaimed;
    epochs.Retire([&] { ++second_reclaimed; });  // E2
    nested_result = epochs.TryReclaim();        // E3: the new object is not yet safe.
  });
  ASSERT_EQ(epochs.TryReclaim(), 0U);  // E1
  ASSERT_EQ(epochs.TryReclaim(), 1U);  // E2, then the callback advances to E3.
  ASSERT_EQ(nested_result, 0U);
  ASSERT_EQ(epochs.CurrentEpoch(), 3U);
  ASSERT_EQ(first_reclaimed, 1);
  ASSERT_EQ(second_reclaimed, 0);
  ASSERT_EQ(epochs.PendingRetired(), 1U);
  ASSERT_EQ(epochs.TryReclaim(), 1U);  // E4
  ASSERT_EQ(first_reclaimed, 1);
  ASSERT_EQ(second_reclaimed, 1);
  ASSERT_EQ(epochs.TryReclaim(), 0U);
  ASSERT_EQ(epochs.PendingRetired(), 0U);
}

TEST(EpochTest, PausedCallbackAllowsOtherThreadsToPinRetireAndCollect) {
  std::cout << "  scenario: a paused callback must not block another thread's metadata operations" << std::endl;
  std::atomic<int> first_reclaimed{0};
  std::atomic<int> second_reclaimed{0};
  std::atomic<int> third_reclaimed{0};
  std::atomic<bool> pause{false};
  std::promise<void> entered;
  auto started = entered.get_future();
  std::promise<void> resume;
  auto may_finish = resume.get_future().share();
  // Captured counters and signals outlive the manager, including failure cleanup.
  EpochManager epochs;
  std::future<std::size_t> collector;
  std::future<void> other;
  std::future_status progress = std::future_status::timeout;
  try {
    epochs.Retire([&, may_finish] {
      ++first_reclaimed;
      if (pause.load()) {
        entered.set_value();
        may_finish.wait();
      }
    });
    ASSERT_EQ(epochs.TryReclaim(), 0U);  // E1
    ASSERT_EQ(first_reclaimed.load(), 0);
    pause.store(true);
    collector = std::async(std::launch::async, [&] { return epochs.TryReclaim(); });  // E2
    ASSERT_EQ(started.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    started.get();

    // All manager operations run in this worker, so a lock-holding callback can be released on timeout.
    other = std::async(std::launch::async, [&] {
      {
        auto reader = epochs.Enter();  // E2
        epochs.Retire([&] { ++second_reclaimed; });
        ASSERT_EQ(epochs.ActiveReaders(), 1U);
        ASSERT_EQ(epochs.PendingRetired(), 1U);  // The paused callback's batch is already detached.
        ASSERT_EQ(epochs.TryReclaim(), 0U);      // E3
        ASSERT_EQ(second_reclaimed.load(), 0);
      }
      ASSERT_EQ(epochs.TryReclaim(), 1U);  // E4: a second collector finishes while the first is paused.
      ASSERT_EQ(second_reclaimed.load(), 1);
      epochs.Retire([&] { ++third_reclaimed; });  // Must remain queued when the paused collector returns.
      ASSERT_EQ(epochs.PendingRetired(), 1U);
    });
    progress = other.wait_for(std::chrono::seconds(5));
  } catch (...) {
    resume.set_value();
    if (collector.valid()) { collector.wait(); }
    if (other.valid()) { other.wait(); }
    throw;
  }
  resume.set_value();  // Always unblock before joining or reporting a failed progress check.
  const auto collected = collector.get();
  other.get();
  ASSERT_EQ(progress, std::future_status::ready);
  ASSERT_EQ(collected, 1U);
  ASSERT_EQ(first_reclaimed.load(), 1);
  ASSERT_EQ(second_reclaimed.load(), 1);
  ASSERT_EQ(third_reclaimed.load(), 0);
  ASSERT_EQ(epochs.ActiveReaders(), 0U);
  ASSERT_EQ(epochs.PendingRetired(), 1U);
  ASSERT_EQ(ReclaimTwoRounds(epochs), 1U);
  ASSERT_EQ(third_reclaimed.load(), 1);
  ASSERT_EQ(epochs.PendingRetired(), 0U);
}

TEST(EpochTest, MixedEpochBatchesReclaimEverySafeObjectOnlyOnce) {
  constexpr std::size_t batch_size = 64;
  std::array<std::atomic<int>, batch_size * 4> calls{};
  for (auto &count : calls) { count.store(0); }
  EpochManager epochs;
  auto retire_batch = [&](std::size_t batch) {
    for (std::size_t i = batch * batch_size; i < (batch + 1) * batch_size; ++i) {
      epochs.Retire([&, i] { ++calls[i]; });
    }
  };
  auto check_calls = [&](std::size_t safe_batches) {
    for (std::size_t i = 0; i < calls.size(); ++i) {
      ASSERT_EQ(calls[i].load(), i < safe_batches * batch_size ? 1 : 0);
    }
  };
  retire_batch(0);  // E0
  ASSERT_TRUE(epochs.TryAdvanceEpoch());
  retire_batch(1);  // E1
  ASSERT_TRUE(epochs.TryAdvanceEpoch());
  retire_batch(2);  // E2
  std::optional<EpochManager::Guard> reader(epochs.Enter());
  ASSERT_TRUE(epochs.TryAdvanceEpoch());
  retire_batch(3);  // E3
  check_calls(0);   // Explicit advancement must not execute any callback.

  ASSERT_TRUE(!epochs.TryAdvanceEpoch());
  ASSERT_EQ(epochs.TryReclaim(), batch_size * 2);  // E0 and E1 are safe even though E3 cannot advance.
  ASSERT_EQ(epochs.CurrentEpoch(), 3U);
  ASSERT_EQ(epochs.PendingRetired(), batch_size * 2);
  check_calls(2);
  ASSERT_EQ(epochs.TryReclaim(), 0U);
  check_calls(2);
  reader.reset();
  ASSERT_EQ(epochs.TryReclaim(), batch_size);  // E4: only E2.
  check_calls(3);
  ASSERT_EQ(epochs.PendingRetired(), batch_size);
  ASSERT_EQ(epochs.TryReclaim(), batch_size);  // E5: E3.
  check_calls(4);
  ASSERT_EQ(epochs.TryReclaim(), 0U);
  ASSERT_EQ(epochs.PendingRetired(), 0U);
}

TEST(EpochTest, LargeBacklogDrainsInTwoRoundsAfterLastReaderExits) {
  constexpr std::size_t total = 4096;
  constexpr std::size_t first_batch = 512;
  std::array<std::atomic<int>, total> calls{};
  for (auto &count : calls) { count.store(0); }
  EpochManager epochs;
  {
    auto old_reader = epochs.Enter();
    for (std::size_t i = 0; i < first_batch; ++i) {
      epochs.Retire([&, i] { ++calls[i]; });
    }
    ASSERT_EQ(epochs.TryReclaim(), 0U);  // E1
    for (std::size_t i = first_batch; i < total; ++i) {
      epochs.Retire([&, i] { ++calls[i]; });
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
      ASSERT_EQ(epochs.TryReclaim(), 0U);
      ASSERT_EQ(epochs.CurrentEpoch(), 1U);
      ASSERT_EQ(epochs.PendingRetired(), total);
    }
    for (const auto &count : calls) { ASSERT_EQ(count.load(), 0); }
  }
  ASSERT_EQ(epochs.ActiveReaders(), 0U);
  ASSERT_EQ(epochs.TryReclaim(), first_batch);  // E2
  for (std::size_t i = 0; i < calls.size(); ++i) {
    ASSERT_EQ(calls[i].load(), i < first_batch ? 1 : 0);
  }
  ASSERT_EQ(epochs.PendingRetired(), total - first_batch);
  ASSERT_EQ(epochs.TryReclaim(), total - first_batch);  // E3
  for (const auto &count : calls) { ASSERT_EQ(count.load(), 1); }
  ASSERT_EQ(epochs.TryReclaim(), 0U);
  ASSERT_EQ(epochs.PendingRetired(), 0U);
}

TEST(EpochTest, IndependentReadersRetirersAndCollectorsKeepExactCounts) {
  constexpr std::size_t per_producer = 512;
  std::array<std::atomic<int>, per_producer * 2> calls{};
  std::array<std::atomic<bool>, per_producer * 2> reading{};
  for (auto &count : calls) { count.store(0); }
  for (auto &flag : reading) { flag.store(false); }
  std::atomic<int> premature{0};
  std::atomic<std::size_t> reported{0};
  EpochManager epochs;
  testutil::Parallel(6, [&](uint32_t thread) {
    if (thread < 2) {
      for (std::size_t i = 0; i < per_producer; ++i) {
        auto guard = epochs.Enter();
        const auto id = thread * per_producer + i;
        reading[id].store(true);
        epochs.Retire([&, id] {
          if (reading[id].load()) { ++premature; }
          ++calls[id];
        });
        if (i % 8 == 0) { std::this_thread::yield(); }
        reading[id].store(false);
      }
    } else if (thread < 4) {
      for (int i = 0; i < 800; ++i) {
        auto outer = epochs.Enter();
        auto inner = epochs.Enter();
        ASSERT_TRUE(epochs.ActiveReaders() >= 2);
        if (i % 8 == 0) { std::this_thread::yield(); }
      }
    } else {
      for (int i = 0; i < 1600; ++i) {
        reported.fetch_add(epochs.TryReclaim());
        if (i % 8 == 0) { std::this_thread::yield(); }
      }
    }
  });
  const auto remaining = ReclaimTwoRounds(epochs);
  ASSERT_EQ(reported.load() + remaining, calls.size());
  ASSERT_EQ(premature.load(), 0);
  for (const auto &count : calls) { ASSERT_EQ(count.load(), 1); }
  ASSERT_EQ(epochs.ActiveReaders(), 0U);
  ASSERT_EQ(epochs.PendingRetired(), 0U);
}

// 集成测试依赖 Task 1 的 Trie；先通过上面的独立 epoch 测试。
void CheckEmpty(EpochTrieStore &store) {
  auto read = store.Read();
  ASSERT_EQ(read.Get<uint32_t>("missing"), nullptr);
}

TEST(EpochTrieStoreTest, OldSnapshotAndLaterReader) {
  std::atomic<int> destroyed{0};
  {
    EpochTrieStore store;
    CheckEmpty(store);
    store.Put<TrackedPtr>("x", std::make_unique<TrackedValue>(destroyed, 1));
    ReclaimTwoRounds(store);
    std::optional<EpochTrieStore::ReadGuard> early(store.Read());
    const auto *old_value = early->Get<TrackedPtr>("x");
    ASSERT_NE(old_value, nullptr);

    store.Put<TrackedPtr>("x", std::make_unique<TrackedValue>(destroyed, 2));
    ASSERT_EQ(store.TryReclaim(), 0U);  // 第一次推进。
    ASSERT_EQ((*old_value)->value_, 1U);
    ASSERT_EQ(destroyed.load(), 0);
    {
      auto nested = store.Read();
      ASSERT_EQ((*nested.Get<TrackedPtr>("x"))->value_, 2U);
      ASSERT_EQ(store.TryReclaim(), 0U);  // 旧读者尚未退出。
    }

    early.reset();  // 从此不能再访问 old_value。
    auto late = store.Read();  // 新的最外层进入，登记当前 epoch。
    ASSERT_EQ(store.TryReclaim(), 1U);
    ASSERT_EQ(destroyed.load(), 1);
    ASSERT_EQ((*late.Get<TrackedPtr>("x"))->value_, 2U);
  }
  ASSERT_EQ(destroyed.load(), 2);
}

TEST(EpochTrieStoreTest, SharedSubtreeSurvivesRootReclamation) {
  std::atomic<int> destroyed{0};
  EpochTrieStore store;
  CheckEmpty(store);
  store.Put<TrackedPtr>("ab", std::make_unique<TrackedValue>(destroyed, 1));
  store.Put<uint32_t>("z", 2);
  ReclaimTwoRounds(store);
  store.Put<uint32_t>("z", 3);
  ASSERT_EQ(ReclaimTwoRounds(store), 1U);
  ASSERT_EQ(destroyed.load(), 0);
  {
    auto read = store.Read();
    ASSERT_EQ((*read.Get<TrackedPtr>("ab"))->value_, 1U);
    ASSERT_EQ(*read.Get<uint32_t>("z"), 3U);
  }
  store.Remove("ab");
  ASSERT_EQ(ReclaimTwoRounds(store), 1U);
  ASSERT_EQ(destroyed.load(), 1);
}

TEST(EpochTrieStoreTest, SnapshotKeepsDeletedAndNoncopyableValues) {
  using Integer = std::unique_ptr<uint32_t>;
  EpochTrieStore store;
  CheckEmpty(store);
  store.Put<Integer>("", std::make_unique<uint32_t>(42));
  ReclaimTwoRounds(store);
  {
    auto read = store.Read();
    const auto *value = read.Get<Integer>("");
    ASSERT_NE(value, nullptr);
    store.Remove("");
    ASSERT_EQ(store.TryReclaim(), 0U);
    ASSERT_EQ(**value, 42U);
    auto latest = store.Read();
    ASSERT_EQ(latest.Get<Integer>(""), nullptr);
  }
  ASSERT_EQ(store.TryReclaim(), 1U);
}

TEST(EpochTrieStoreTest, ConcurrentUpdatesAndReclamation) {
  EpochTrieStore store;
  CheckEmpty(store);
  std::vector<std::future<void>> workers;
  for (int thread = 0; thread < 4; ++thread) {
    workers.push_back(std::async(std::launch::async, [&, thread] {
      auto key = std::to_string(thread);
      for (uint32_t i = 0; i < 200; ++i) {
        store.Put<uint32_t>(key, i);
        auto read = store.Read();
        const auto *value = read.Get<uint32_t>(key);
        ASSERT_NE(value, nullptr);
        store.Put<uint32_t>(key, i + 1000);
        store.TryReclaim();
        ASSERT_EQ(*value, i);
      }
    }));
  }
  for (auto &worker : workers) {
    worker.get();
  }
  ReclaimTwoRounds(store);
  ASSERT_EQ(store.ActiveReaders(), 0U);
  ASSERT_EQ(store.PendingRetired(), 0U);
  auto read = store.Read();
  for (int thread = 0; thread < 4; ++thread) {
    ASSERT_EQ(*read.Get<uint32_t>(std::to_string(thread)), 1199U);
  }
}

TEST(EpochTrieStoreTest, ReadGuardTransferredToWorkerKeepsRetiredVersionAlive) {
  std::cout << "  scenario: a moved ReadGuard protects an overwritten and deleted value on another thread" << std::endl;
  std::atomic<int> old_destroyed{0};
  std::atomic<int> new_destroyed{0};
  std::promise<void> entered;
  auto ready = entered.get_future();
  std::promise<void> release;
  auto may_exit = release.get_future();
  EpochTrieStore store;
  store.Put<TrackedPtr>("key", std::make_unique<TrackedValue>(old_destroyed, 1));
  ReclaimTwoRounds(store);
  auto original = store.Read();
  auto moved = std::move(original);
  ASSERT_EQ(original.GetRoot(), nullptr);
  ASSERT_EQ(original.Get<TrackedPtr>("key"), nullptr);

  auto worker = std::async(std::launch::async, [&, transferred = std::move(moved)]() mutable {
    std::optional<EpochTrieStore::ReadGuard> owner(std::move(transferred));
    const TrackedPtr *value = nullptr;
    try {
      value = owner->Get<TrackedPtr>("key");
      ASSERT_NE(value, nullptr);
      ASSERT_NE(value->get(), nullptr);
      ASSERT_EQ((*value)->value_, 1U);
    } catch (...) {
      entered.set_exception(std::current_exception());
      throw;
    }
    entered.set_value();
    may_exit.wait();
    ASSERT_EQ(old_destroyed.load(), 0);
    ASSERT_EQ((*value)->value_, 1U);
    owner.reset();  // Release the original thread's Pin in this worker.
  });
  try {
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ready.get();
    ASSERT_EQ(moved.GetRoot(), nullptr);
    ASSERT_EQ(store.ActiveReaders(), 1U);
    store.Put<TrackedPtr>("key", std::make_unique<TrackedValue>(new_destroyed, 2));
    ASSERT_EQ(store.TryReclaim(), 0U);
    store.Remove("key");
    {
      auto latest = store.Read();
      ASSERT_EQ(latest.Get<TrackedPtr>("key"), nullptr);
    }
    ASSERT_EQ(store.TryReclaim(), 0U);
    ASSERT_EQ(store.PendingRetired(), 2U);
    ASSERT_EQ(old_destroyed.load(), 0);
    ASSERT_EQ(new_destroyed.load(), 0);
  } catch (...) {
    release.set_value();
    worker.wait();
    throw;
  }
  release.set_value();
  worker.get();
  ASSERT_EQ(store.ActiveReaders(), 0U);
  ASSERT_EQ(ReclaimTwoRounds(store), 2U);
  ASSERT_EQ(old_destroyed.load(), 1);
  ASSERT_EQ(new_destroyed.load(), 1);
  ASSERT_EQ(store.PendingRetired(), 0U);
}

TEST(EpochTrieStoreTest, StaggeredReadersReleaseVersionsAndSharedValuesInStages) {
  std::cout << "  scenario: readers in different epochs release old roots in stages while sharing a subtree" << std::endl;
  std::array<std::atomic<int>, 4> destroyed{};
  for (auto &count : destroyed) { count.store(0); }
  std::promise<void> entered;
  auto ready = entered.get_future();
  std::promise<void> release;
  auto may_exit = release.get_future();
  EpochTrieStore store;
  store.Put<TrackedPtr>("stable", std::make_unique<TrackedValue>(destroyed[0], 10));
  store.Put<TrackedPtr>("changing", std::make_unique<TrackedValue>(destroyed[1], 1));
  ReclaimTwoRounds(store);  // E2
  std::optional<EpochTrieStore::ReadGuard> early(store.Read());
  store.Put<TrackedPtr>("changing", std::make_unique<TrackedValue>(destroyed[2], 2));
  ASSERT_EQ(store.TryReclaim(), 0U);  // E3

  auto worker = std::async(std::launch::async, [&] {
    std::optional<EpochTrieStore::ReadGuard> later;
    try {
      later.emplace(store.Read());  // A different thread: genuinely enters E3, rather than nesting at E2.
      const auto *stable = later->Get<TrackedPtr>("stable");
      const auto *changing = later->Get<TrackedPtr>("changing");
      ASSERT_NE(stable, nullptr);
      ASSERT_NE(changing, nullptr);
      ASSERT_NE(stable->get(), nullptr);
      ASSERT_NE(changing->get(), nullptr);
      ASSERT_EQ((*stable)->value_, 10U);
      ASSERT_EQ((*changing)->value_, 2U);
    } catch (...) {
      entered.set_exception(std::current_exception());
      throw;
    }
    entered.set_value();
    may_exit.wait();
    ASSERT_EQ(destroyed[0].load(), 0);
    ASSERT_EQ(destroyed[2].load(), 0);
    ASSERT_EQ((*later->Get<TrackedPtr>("stable"))->value_, 10U);
    ASSERT_EQ((*later->Get<TrackedPtr>("changing"))->value_, 2U);
    later.reset();
  });
  try {
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ready.get();
    ASSERT_EQ(store.ActiveReaders(), 2U);
    store.Put<TrackedPtr>("changing", std::make_unique<TrackedValue>(destroyed[3], 3));
    store.Remove("stable");
    ASSERT_EQ(store.TryReclaim(), 0U);  // The E2 reader still blocks E3 -> E4.
    for (const auto &count : destroyed) { ASSERT_EQ(count.load(), 0); }
    early.reset();

    ASSERT_EQ(store.TryReclaim(), 1U);  // E4: only the E2 root is safe.
    ASSERT_EQ(destroyed[1].load(), 1);
    ASSERT_EQ(destroyed[0].load(), 0);  // Still shared by the E3 roots.
    ASSERT_EQ(destroyed[2].load(), 0);
    ASSERT_EQ(destroyed[3].load(), 0);
    ASSERT_EQ(store.TryReclaim(), 0U);  // E3 reader prevents E4 -> E5.
    ASSERT_EQ(store.PendingRetired(), 2U);
  } catch (...) {
    release.set_value();
    worker.wait();
    throw;
  }
  release.set_value();
  worker.get();
  ASSERT_EQ(store.ActiveReaders(), 0U);
  ASSERT_EQ(store.TryReclaim(), 2U);  // E5: both E3 roots are now safe.
  ASSERT_EQ(destroyed[0].load(), 1);
  ASSERT_EQ(destroyed[1].load(), 1);
  ASSERT_EQ(destroyed[2].load(), 1);
  ASSERT_EQ(destroyed[3].load(), 0);  // The current root still owns this value.
  {
    auto latest = store.Read();
    ASSERT_EQ(latest.Get<TrackedPtr>("stable"), nullptr);
    const auto *value = latest.Get<TrackedPtr>("changing");
    ASSERT_NE(value, nullptr);
    ASSERT_NE(value->get(), nullptr);
    ASSERT_EQ((*value)->value_, 3U);
  }
  store.Remove("changing");
  ASSERT_EQ(ReclaimTwoRounds(store), 1U);
  for (const auto &count : destroyed) { ASSERT_EQ(count.load(), 1); }
  ASSERT_EQ(store.PendingRetired(), 0U);
}

}  // namespace trie

int main(int argc, char **argv) { return labtest::Run(argc, argv); }
