// Adapted from CMU BusTub v20231227-2023fall; see LICENSE.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/move_blocked.h"
#include "common/tracked_value.h"
#include "test.h"
#include "test_utils.h"
#include "trie_store.h"

namespace trie {
using Integer = std::unique_ptr<uint32_t>;

// 相同的行为测试同时验收互斥锁版本和 CAS 版本。
#define STORE_TEST(name)                              \
  template <class Store> void Check##name();           \
  TEST(TrieStoreTest, name) { Check##name<TrieStore>(); } \
  TEST(CasStoreTest, name) { Check##name<CasTrieStore>(); } \
  template <class Store> void Check##name()

STORE_TEST(BasicTest) {
  auto store = Store();
  ASSERT_EQ(store.template Get<uint32_t>("233"), std::nullopt);
  store.template Put<uint32_t>("233", 2333);
  {
    auto guard = store.template Get<uint32_t>("233");
    ASSERT_EQ(**guard, 2333);
  }
  store.Remove("233");
  {
    auto guard = store.template Get<uint32_t>("233");
    ASSERT_EQ(guard, std::nullopt);
  }
}

STORE_TEST(GuardTest) {
  auto store = Store();
  ASSERT_EQ(store.template Get<uint32_t>("233"), std::nullopt);

  store.template Put<std::string>("233", "2333");
  auto guard = store.template Get<std::string>("233");
  ASSERT_EQ(**guard, "2333");

  store.template Put<std::string>("233", "updated");
  ASSERT_EQ(**guard, "2333");
  store.Remove("233");
  {
    auto guard = store.template Get<std::string>("233");
    ASSERT_EQ(guard, std::nullopt);
  }

  ASSERT_EQ(**guard, "2333");
}

STORE_TEST(MixedTest) {
  auto store = Store();
  for (uint32_t i = 0; i < 23333; i++) {
    std::string key = labtest::Pad(i, 5);
    std::string value = "value-" + labtest::Pad(i, 8);
    store.template Put<std::string>(key, value);
  }
  for (uint32_t i = 0; i < 23333; i += 2) {
    std::string key = labtest::Pad(i, 5);
    std::string value = "new-value-" + labtest::Pad(i, 8);
    store.template Put<std::string>(key, value);
  }
  for (uint32_t i = 0; i < 23333; i += 3) {
    std::string key = labtest::Pad(i, 5);
    store.Remove(key);
  }

  // verify final trie
  for (uint32_t i = 0; i < 23333; i++) {
    std::string key = labtest::Pad(i, 5);
    if (i % 3 == 0) {
      ASSERT_EQ(store.template Get<std::string>(key), std::nullopt);
    } else if (i % 2 == 0) {
      std::string value = "new-value-" + labtest::Pad(i, 8);
      auto guard = store.template Get<std::string>(key);
      ASSERT_EQ(**guard, value);
    } else {
      std::string value = "value-" + labtest::Pad(i, 8);
      auto guard = store.template Get<std::string>(key);
      ASSERT_EQ(**guard, value);
    }
  }
}

STORE_TEST(MixedConcurrentTest) {
  Store store;
  constexpr uint32_t keys_per_thread = 10000;
  std::atomic<bool> stop{false};
  std::promise<void> start;
  auto ready = start.get_future().share();
  std::vector<std::future<void>> writers;
  std::vector<std::future<void>> readers;
  for (uint32_t tid = 0; tid < 4; ++tid) {
    writers.push_back(std::async(std::launch::async, [&, tid] {
      ready.wait();
      for (uint32_t i = 0; i < keys_per_thread; ++i) {
        auto id = i * 4 + tid;
        store.template Put<std::string>(labtest::Pad(id, 5), "value-" + labtest::Pad(id, 8));
      }
      for (uint32_t i = 0; i < keys_per_thread; ++i) {
        store.Remove(labtest::Pad(i * 4 + tid, 5));
      }
      for (uint32_t i = 0; i < keys_per_thread; ++i) {
        auto id = i * 4 + tid;
        store.template Put<std::string>(labtest::Pad(id, 5), "new-value-" + labtest::Pad(id, 8));
      }
    }));
    readers.push_back(std::async(std::launch::async, [&, tid] {
      ready.wait();
      uint32_t i = 0;
      while (!stop.load()) {
        auto id = i * 4 + tid;
        auto guard = store.template Get<std::string>(labtest::Pad(id, 5));
        if (guard) {
          ASSERT_TRUE(**guard == "value-" + labtest::Pad(id, 8) ||
                      **guard == "new-value-" + labtest::Pad(id, 8));
        }
        i = (i + 1) % keys_per_thread;
      }
    }));
  }
  start.set_value();
  std::exception_ptr error;
  auto finish = [&error](std::vector<std::future<void>> &workers) {
    for (auto &worker : workers) {
      try {
        worker.get();
      } catch (...) {
        error = std::current_exception();
      }
    }
  };
  finish(writers);
  stop.store(true);
  finish(readers);
  if (error) {
    std::rethrow_exception(error);
  }
  for (uint32_t i = 0; i < keys_per_thread * 4; ++i) {
    auto guard = store.template Get<std::string>(labtest::Pad(i, 5));
    ASSERT_TRUE(guard.has_value());
    ASSERT_EQ(**guard, "new-value-" + labtest::Pad(i, 8));
  }
}

STORE_TEST(NonCopyableTest) {
  auto store = Store();
  store.template Put<Integer>("tes", std::make_unique<uint32_t>(233));
  store.template Put<Integer>("te", std::make_unique<uint32_t>(23));
  store.template Put<Integer>("test", std::make_unique<uint32_t>(2333));
  ASSERT_EQ(***store.template Get<Integer>("te"), 23);
  ASSERT_EQ(***store.template Get<Integer>("tes"), 233);
  ASSERT_EQ(***store.template Get<Integer>("test"), 2333);
  store.Remove("te");
  store.Remove("tes");
  store.Remove("test");
  ASSERT_EQ(store.template Get<Integer>("te"), std::nullopt);
  ASSERT_EQ(store.template Get<Integer>("tes"), std::nullopt);
  ASSERT_EQ(store.template Get<Integer>("test"), std::nullopt);
}

STORE_TEST(ReadWriteTest) {
  Store store;
  store.template Put<uint32_t>("a", 1);
  store.template Put<uint32_t>("b", 2);
  store.template Put<uint32_t>("c", 3);
  std::promise<int> resume;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  auto writer = std::async(std::launch::async, [&] {
    store.template Put<MoveBlocked>("d", MoveBlocked(resume.get_future(), &entered));
  });

  // 等到写者确实进入阻塞的移动构造，再检查读者能否完成。
  auto entered_status = entered_future.wait_for(std::chrono::seconds(5));
  if (entered_status != std::future_status::ready) {
    resume.set_value(233);
    writer.get();
    ASSERT_EQ(entered_status, std::future_status::ready);
  }
  auto reader = std::async(std::launch::async, [&] {
    uint32_t expected = 1;
    for (const auto *key : {"a", "b", "c"}) {
      auto guard = store.template Get<uint32_t>(key);
      ASSERT_TRUE(guard.has_value());
      ASSERT_EQ(**guard, expected++);
    }
  });
  auto read_status = reader.wait_for(std::chrono::seconds(5));
  // 即使实现错误导致读者阻塞，也先释放写者，再报告断言失败。
  resume.set_value(233);
  writer.get();
  reader.get();
  ASSERT_EQ(read_status, std::future_status::ready);
  ASSERT_TRUE(store.template Get<MoveBlocked>("d").has_value());
}

STORE_TEST(EmptyKeyAndTypeTest) {
  Store store;
  store.template Put<uint32_t>("", 7);
  auto old = store.template Get<uint32_t>("");
  ASSERT_TRUE(old.has_value());
  ASSERT_EQ(**old, 7U);
  ASSERT_EQ(store.template Get<std::string>(""), std::nullopt);
  store.template Put<std::string>("", "root");
  ASSERT_EQ(store.template Get<uint32_t>(""), std::nullopt);
  ASSERT_EQ(**old, 7U);
  store.Remove("missing");
  auto current = store.template Get<std::string>("");
  ASSERT_TRUE(current.has_value());
  ASSERT_EQ(**current, "root");
  store.Remove("");
  ASSERT_EQ(store.template Get<std::string>(""), std::nullopt);
  ASSERT_EQ(**old, 7U);
}

STORE_TEST(FailedWritePreservesCurrentVersion) {
  std::atomic<int> destroyed{0};
  Store store;
  store.template Put<TrackedPtr>("x", std::make_unique<TrackedValue>(destroyed, 7));
  std::promise<int> failure;
  failure.set_exception(std::make_exception_ptr(std::runtime_error("value construction failed")));
  bool caught = false;
  try {
    store.template Put<MoveBlocked>("x", MoveBlocked(failure.get_future()));
  } catch (const std::runtime_error &) {
    caught = true;
  }
  ASSERT_TRUE(caught);
  ASSERT_EQ((**store.template Get<TrackedPtr>("x"))->value_, 7U);
  ASSERT_EQ(destroyed.load(), 0);
  store.Remove("x");
  ASSERT_EQ(destroyed.load(), 1);
  store.template Put<uint32_t>("x", 8);  // 异常退出后锁也必须正常释放。
  ASSERT_EQ(**store.template Get<uint32_t>("x"), 8U);
}

STORE_TEST(SharedSubtreeSurvivesOldSnapshots) {
  std::atomic<int> destroyed{0};
  Store store;
  store.template Put<TrackedPtr>("ab", std::make_unique<TrackedValue>(destroyed, 7));
  auto old = store.template Get<TrackedPtr>("ab");
  store.template Put<uint32_t>("z", 8);
  old.reset();  // 旧版本回收，新版本共享的值仍应存活。
  ASSERT_EQ(destroyed.load(), 0);
  auto current = store.template Get<TrackedPtr>("ab");
  ASSERT_TRUE(current.has_value());
  ASSERT_EQ((**current)->value_, 7U);
  store.Remove("ab");
  ASSERT_EQ(destroyed.load(), 0);
  current.reset();
  ASSERT_EQ(destroyed.load(), 1);
  ASSERT_EQ(**store.template Get<uint32_t>("z"), 8U);
}

STORE_TEST(GuardCopiesKeepOldValueAlive) {
  std::atomic<int> old_destroyed{0};
  std::atomic<int> new_destroyed{0};
  Store store;
  store.template Put<TrackedPtr>("x", std::make_unique<TrackedValue>(old_destroyed, 1));
  auto first = store.template Get<TrackedPtr>("x");
  auto second = store.template Get<TrackedPtr>("x");
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  auto copied = first;  // 复制 guard 也必须延长旧版本的生命周期。

  store.template Put<TrackedPtr>("x", std::make_unique<TrackedValue>(new_destroyed, 2));
  auto latest = store.template Get<TrackedPtr>("x");
  ASSERT_TRUE(latest.has_value());
  store.Remove("x");
  ASSERT_EQ(store.template Get<TrackedPtr>("x"), std::nullopt);
  ASSERT_EQ(old_destroyed.load(), 0);
  ASSERT_EQ(new_destroyed.load(), 0);
  ASSERT_EQ((**first)->value_, 1U);
  ASSERT_EQ((**latest)->value_, 2U);

  first.reset();
  second.reset();
  ASSERT_EQ(old_destroyed.load(), 0);
  ASSERT_EQ((**copied)->value_, 1U);
  copied.reset();
  ASSERT_EQ(old_destroyed.load(), 1);
  ASSERT_EQ(new_destroyed.load(), 0);  // 新版本的读者不阻止旧版本回收。
  latest.reset();
  ASSERT_EQ(new_destroyed.load(), 1);
}

STORE_TEST(FailedLookupsDoNotKeepSnapshots) {
  std::atomic<int> destroyed{0};
  Store store;
  store.template Put<TrackedPtr>("x", std::make_unique<TrackedValue>(destroyed, 1));
  ASSERT_EQ(store.template Get<uint32_t>("x"), std::nullopt);
  ASSERT_EQ(store.template Get<TrackedPtr>("missing"), std::nullopt);
  store.template Put<TrackedPtr>("x", std::make_unique<TrackedValue>(destroyed, 2));
  ASSERT_EQ(destroyed.load(), 1);
  store.Remove("x");
  ASSERT_EQ(destroyed.load(), 2);
}

STORE_TEST(GuardSurvivesStoreDestruction) {
  std::atomic<int> destroyed{0};
  std::optional<ValueGuard<TrackedPtr>> reader;
  {
    Store store;
    store.template Put<TrackedPtr>("x", std::make_unique<TrackedValue>(destroyed, 7));
    auto acquired = store.template Get<TrackedPtr>("x");
    ASSERT_TRUE(acquired.has_value());
    reader.emplace(std::move(*acquired));
  }
  ASSERT_EQ(destroyed.load(), 0);
  ASSERT_EQ((**reader)->value_, 7U);
  reader.reset();
  ASSERT_EQ(destroyed.load(), 1);
}

STORE_TEST(ConcurrentSnapshotsAndUpdates) {
  constexpr uint32_t updates = 1000;
  std::atomic<int> destroyed{0};
  Store store;
  store.template Put<TrackedPtr>("x", std::make_unique<TrackedValue>(destroyed, 0));
  std::promise<void> start;
  auto ready = start.get_future().share();
  std::vector<std::future<void>> workers;
  workers.push_back(std::async(std::launch::async, [&] {
    ready.wait();
    for (uint32_t i = 1; i <= updates; ++i) {
      store.template Put<TrackedPtr>("x", std::make_unique<TrackedValue>(destroyed, i));
      if (i % 7 == 0) {
        store.Remove("x");
      }
    }
    store.Remove("x");
  }));
  for (int thread = 0; thread < 4; ++thread) {
    workers.push_back(std::async(std::launch::async, [&] {
      ready.wait();
      for (uint32_t i = 0; i < updates * 2; ++i) {
        auto guard = store.template Get<TrackedPtr>("x");
        if (guard) {
          const auto *value = (**guard).get();
          const auto before = value->value_;
          std::this_thread::yield();
          ASSERT_EQ(value->value_, before);
          ASSERT_TRUE(value->value_ <= updates);
        }
      }
    }));
  }
  start.set_value();
  for (auto &worker : workers) {
    worker.get();
  }
  ASSERT_EQ(store.template Get<TrackedPtr>("x"), std::nullopt);
  ASSERT_EQ(destroyed.load(), static_cast<int>(updates + 1));
}

#undef STORE_TEST

TEST(CasStoreTest, ConflictingPutRetriesWithoutLosingValue) {
  CasTrieStore store;
  store.Put<uint32_t>("base", 1);
  std::promise<int> resume;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  auto slow = std::async(std::launch::async, [&] {
    store.Put<MoveBlocked>("slow", MoveBlocked(resume.get_future(), &entered, 7));
  });
  auto entered_status = entered_future.wait_for(std::chrono::seconds(5));
  if (entered_status != std::future_status::ready) {
    resume.set_value(0);
    slow.get();
    ASSERT_EQ(entered_status, std::future_status::ready);
  }
  auto fast = std::async(std::launch::async, [&] { store.Put<uint32_t>("fast", 8); });
  auto fast_status = fast.wait_for(std::chrono::seconds(5));
  // 无论断言是否成功，都先释放暂停的写者，避免错误实现让测试自身死锁。
  resume.set_value(0);
  slow.get();
  fast.get();
  ASSERT_EQ(fast_status, std::future_status::ready);
  auto value = store.Get<MoveBlocked>("slow");
  ASSERT_TRUE(value.has_value());
  ASSERT_NE((**value).GetValue(), nullptr);
  ASSERT_EQ(*(**value).GetValue(), 7U);
  ASSERT_EQ(**store.Get<uint32_t>("fast"), 8U);
  ASSERT_EQ(**store.Get<uint32_t>("base"), 1U);
}

TEST(CasStoreTest, ConflictingRemoveIsNotUndoneByRetry) {
  CasTrieStore store;
  store.Put<uint32_t>("gone", 1);
  std::promise<int> resume;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  auto slow = std::async(std::launch::async, [&] {
    store.Put<MoveBlocked>("new", MoveBlocked(resume.get_future(), &entered, 9));
  });
  auto entered_status = entered_future.wait_for(std::chrono::seconds(5));
  if (entered_status != std::future_status::ready) {
    resume.set_value(0);
    slow.get();
    ASSERT_EQ(entered_status, std::future_status::ready);
  }
  auto remover = std::async(std::launch::async, [&] { store.Remove("gone"); });
  auto remove_status = remover.wait_for(std::chrono::seconds(5));
  resume.set_value(0);
  slow.get();
  remover.get();
  ASSERT_EQ(remove_status, std::future_status::ready);
  ASSERT_EQ(store.Get<uint32_t>("gone"), std::nullopt);
  auto value = store.Get<MoveBlocked>("new");
  ASSERT_TRUE(value.has_value());
  ASSERT_NE((**value).GetValue(), nullptr);
  ASSERT_EQ(*(**value).GetValue(), 9U);
}

TEST(CasStoreTest, WritersBuildInParallelWithMoveOnlyValues) {
  CasTrieStore store;
  std::promise<int> resume_first;
  std::promise<int> resume_second;
  std::promise<void> first_entered;
  std::promise<void> second_entered;
  auto first_ready = first_entered.get_future();
  auto second_ready = second_entered.get_future();
  auto first = std::async(std::launch::async, [&] {
    store.Put<MoveBlocked>("a", MoveBlocked(resume_first.get_future(), &first_entered, 41));
  });
  auto second = std::async(std::launch::async, [&] {
    store.Put<MoveBlocked>("ab", MoveBlocked(resume_second.get_future(), &second_entered, 42));
  });
  auto first_status = first_ready.wait_for(std::chrono::seconds(5));
  auto second_status = second_ready.wait_for(std::chrono::seconds(5));
  resume_first.set_value(0);
  resume_second.set_value(0);
  first.get();
  second.get();
  ASSERT_EQ(first_status, std::future_status::ready);
  ASSERT_EQ(second_status, std::future_status::ready);
  auto a = store.Get<MoveBlocked>("a");
  auto ab = store.Get<MoveBlocked>("ab");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(ab.has_value());
  ASSERT_NE((**a).GetValue(), nullptr);
  ASSERT_NE((**ab).GetValue(), nullptr);
  ASSERT_EQ(*(**a).GetValue(), 41U);
  ASSERT_EQ(*(**ab).GetValue(), 42U);
}

TEST(CasStoreTest, ConcurrentRemovesPreserveInsertedKeys) {
  constexpr uint32_t count = 400;
  std::atomic<int> destroyed{0};
  CasTrieStore store;
  for (uint32_t i = 0; i < count; ++i) {
    store.Put<uint32_t>("old/" + std::to_string(i), i);
  }
  std::promise<void> start;
  auto ready = start.get_future().share();
  std::vector<std::future<void>> workers;
  for (uint32_t offset = 0; offset < 2; ++offset) {
    workers.push_back(std::async(std::launch::async, [&, offset] {
      ready.wait();
      for (uint32_t i = offset; i < count; i += 2) {
        store.Remove("old/" + std::to_string(i));
      }
    }));
  }
  workers.push_back(std::async(std::launch::async, [&] {
    ready.wait();
    for (uint32_t i = 0; i < count; ++i) {
      store.Put<TrackedPtr>("new/" + std::to_string(i), std::make_unique<TrackedValue>(destroyed, i));
    }
  }));
  start.set_value();
  for (auto &worker : workers) {
    worker.get();
  }
  for (uint32_t i = 0; i < count; ++i) {
    ASSERT_EQ(store.Get<uint32_t>("old/" + std::to_string(i)), std::nullopt);
    auto value = store.Get<TrackedPtr>("new/" + std::to_string(i));
    ASSERT_TRUE(value.has_value());
    ASSERT_NE((**value).get(), nullptr);
    ASSERT_EQ((**value)->value_, i);
  }
  ASSERT_EQ(destroyed.load(), 0);
  for (uint32_t i = 0; i < count; ++i) {
    store.Remove("new/" + std::to_string(i));
  }
  ASSERT_EQ(destroyed.load(), static_cast<int>(count));
}

#define STORE_TEST(name)                                       \
  template <class Store> void Check##name();                             \
  TEST(TrieStoreTest, name) { Check##name<TrieStore>(); }                 \
  TEST(CasStoreTest, name) { Check##name<CasTrieStore>(); }                 \
  template <class Store> void Check##name()

STORE_TEST(BinaryKeysAndMixedTypesAgainstOracle) {
  Store store;
  testutil::Model model;
  const auto keys = testutil::Keys();
  std::mt19937 random(645);
  for (uint32_t step = 0; step < 3000; ++step) {
    const auto &key = keys[random() % keys.size()];
    switch (random() % 3) {
      case 0:
        store.template Put<uint32_t>(key, step);
        model[key] = step;
        break;
      case 1: {
        auto value = "value/" + std::to_string(step);
        store.template Put<std::string>(key, value);
        model[key] = value;
        break;
      }
      default:
        store.Remove(key);
        model.erase(key);
    }
    if (step % 50 == 0) { testutil::CheckStore(store, model, keys); }
  }
  testutil::CheckStore(store, model, keys);
}

STORE_TEST(ConcurrentWritersWithSharedPrefixes) {
  Store store;
  store.template Put<uint32_t>("", 12345);
  auto key = [](uint32_t thread, uint32_t item) {
    return "shared/" + std::to_string(thread) + "/" + std::string(1, static_cast<char>(item)) + "/leaf";
  };
  testutil::Parallel(6, [&](uint32_t thread) {
    if (thread < 4) {
      for (uint32_t round = 0; round < 8; ++round) {
        for (uint32_t i = 0; i < 48; ++i) {
          store.template Put<uint32_t>(key(thread, i), round * 100 + i);
          if (i % 2 != 0) { store.template Put<std::string>(key(thread, i), "round/" + std::to_string(round)); }
          if (i % 3 == 0) { store.Remove(key(thread, i)); }
        }
      }
    } else {
      for (uint32_t i = 0; i < 5000; ++i) {
        auto sentinel = store.template Get<uint32_t>("");
        ASSERT_TRUE(sentinel.has_value());
        ASSERT_EQ(**sentinel, 12345U);
        auto guard = store.template Get<uint32_t>(key(i % 4, i % 48));
        if (guard) {
          const auto value = **guard;
          std::this_thread::yield();
          ASSERT_EQ(**guard, value);
          ASSERT_TRUE(value < 800);
        }
      }
    }
  });
  for (uint32_t thread = 0; thread < 4; ++thread) {
    for (uint32_t i = 0; i < 48; ++i) {
      auto integer = store.template Get<uint32_t>(key(thread, i));
      auto text = store.template Get<std::string>(key(thread, i));
      if (i % 3 == 0) {
        ASSERT_TRUE(!integer && !text);
      } else if (i % 2 == 0) {
        ASSERT_TRUE(integer.has_value());
        ASSERT_EQ(**integer, 700 + i);
        ASSERT_TRUE(!text);
      } else {
        ASSERT_TRUE(text.has_value());
        ASSERT_EQ(**text, "round/7");
        ASSERT_TRUE(!integer);
      }
    }
  }
}

struct Operation {
  enum Kind { Read, Write, Erase } kind;
  uint32_t key;
  uint32_t value;
  uint32_t begin{0};
  uint32_t end{0};
  int64_t result{-1};
};

// Exhaustively check a small history against a sequential three-key map.
// begin/end enforce real-time order for operations that did not overlap.
auto IsLinearizable(const std::array<Operation, 12> &history,
                    const std::array<int64_t, 3> &final_state) -> bool {
  std::array<uint32_t, 12> before{};
  for (std::size_t i = 0; i < history.size(); ++i) {
    for (std::size_t j = 0; j < history.size(); ++j) {
      if (history[j].end < history[i].begin) { before[i] |= 1U << j; }
    }
  }
  std::set<std::pair<uint32_t, std::array<int64_t, 3>>> rejected;
  auto search = [&](auto &&self, uint32_t done, std::array<int64_t, 3> state) -> bool {
    if (done == (1U << history.size()) - 1) { return state == final_state; }
    if (!rejected.emplace(done, state).second) { return false; }
    for (std::size_t i = 0; i < history.size(); ++i) {
      if ((done & (1U << i)) != 0 || (done & before[i]) != before[i]) { continue; }
      const auto &op = history[i];
      auto next = state;
      if (op.kind == Operation::Read) {
        if (state[op.key] != op.result) { continue; }
      } else {
        next[op.key] = op.kind == Operation::Write ? static_cast<int64_t>(op.value) : -1;
      }
      if (self(self, done | (1U << i), next)) { return true; }
    }
    return false;
  };
  return search(search, 0, {1, -1, -1});
}

STORE_TEST(SmallHistoriesAreLinearizable) {
  std::mt19937 random(1445);
  const std::array<std::string, 3> keys{"", "x", "xy"};
  for (uint32_t round = 0; round < 100; ++round) {
    Store store;
    store.template Put<uint32_t>(keys[0], 1);
    std::array<Operation, 12> history{};
    for (uint32_t i = 0; i < history.size(); ++i) {
      history[i] = {static_cast<Operation::Kind>(random() % 3),
                    static_cast<uint32_t>(random() % keys.size()), round * 16 + i + 2};
    }
    std::atomic<uint32_t> clock{0};
    testutil::Parallel(3, [&](uint32_t thread) {
      for (uint32_t step = 0; step < 4; ++step) {
        auto &op = history[thread * 4 + step];
        op.begin = clock.fetch_add(1);
        std::this_thread::yield();
        if (op.kind == Operation::Write) {
          store.template Put<uint32_t>(keys[op.key], op.value);
        } else if (op.kind == Operation::Erase) {
          store.Remove(keys[op.key]);
        } else {
          auto value = store.template Get<uint32_t>(keys[op.key]);
          op.result = value ? static_cast<int64_t>(**value) : -1;
        }
        op.end = clock.fetch_add(1);
      }
    });
    std::array<int64_t, 3> final_state{};
    for (std::size_t i = 0; i < keys.size(); ++i) {
      auto value = store.template Get<uint32_t>(keys[i]);
      final_state[i] = value ? static_cast<int64_t>(**value) : -1;
    }
    const bool valid = IsLinearizable(history, final_state);
    if (!valid) {
      std::cerr << "seed=1445, round=" << round << '\n';
      for (const auto &op : history) {
        std::cerr << "kind=" << op.kind << " key=" << op.key << " value=" << op.value
                  << " interval=[" << op.begin << ',' << op.end << "] result=" << op.result << '\n';
      }
    }
    ASSERT_TRUE(valid);
  }
}

STORE_TEST(HotKeyMoveOnlyValuesAndDelayedReaders) {
  std::atomic<int> destroyed{0};
  {
    Store store;
    testutil::Parallel(4, [&](uint32_t thread) {
      std::array<std::optional<ValueGuard<TrackedPtr>>, 16> guards;
      std::array<uint32_t, 16> values{};
      for (uint32_t i = 0; i < 300; ++i) {
        store.template Put<TrackedPtr>("hot", std::make_unique<TrackedValue>(destroyed, thread * 300 + i));
        auto &held = guards[i % guards.size()];
        held.reset();
        auto current = store.template Get<TrackedPtr>("hot");
        if (current) {
          ASSERT_NE((**current).get(), nullptr);
          held.emplace(std::move(*current));
          values[i % guards.size()] = (**held)->value_;
          ASSERT_TRUE((**held)->value_ < 1200);
        }
        if (i % 7 == 0) { store.Remove("hot"); }
        std::this_thread::yield();
        for (std::size_t slot = 0; slot < guards.size(); ++slot) {
          if (guards[slot]) { ASSERT_EQ((**guards[slot])->value_, values[slot]); }
        }
      }
    });
    store.Remove("hot");
  }
  ASSERT_EQ(destroyed.load(), 1200);
}

STORE_TEST(GuardTransferAfterStoreDestruction) {
  std::atomic<int> destroyed{0};
  std::vector<ValueGuard<TrackedPtr>> guards;
  {
    Store store;
    for (uint32_t i = 0; i < 16; ++i) {
      store.template Put<TrackedPtr>("x", std::make_unique<TrackedValue>(destroyed, i));
      auto value = store.template Get<TrackedPtr>("x");
      ASSERT_TRUE(value.has_value());
      guards.push_back(*value);
    }
  }
  ASSERT_EQ(destroyed.load(), 0);
  // The Store no longer exists. Guard copies can still be passed to other threads.
  testutil::Parallel(4, [&](uint32_t thread) {
    for (uint32_t repeat = 0; repeat < 100; ++repeat) {
      for (std::size_t i = thread; i < guards.size(); i += 4) {
        auto copy = guards[i];
        ASSERT_EQ((*copy)->value_, i);
      }
    }
  });
  guards.clear();
  ASSERT_EQ(destroyed.load(), 16);
}

STORE_TEST(ExceptionsDoNotBlockOtherWriters) {
  Store store;
  store.template Put<uint32_t>("failed", 7);
  testutil::Parallel(4, [&](uint32_t thread) {
    for (uint32_t i = 0; i < 80; ++i) {
      if (thread % 2 == 0) {
        std::promise<int> failure;
        failure.set_exception(std::make_exception_ptr(std::runtime_error("intentional move failure")));
        bool caught = false;
        try {
          store.template Put<MoveBlocked>("failed", MoveBlocked(failure.get_future()));
        } catch (const std::runtime_error &) {
          caught = true;
        }
        ASSERT_TRUE(caught);
      } else {
        store.template Put<uint32_t>("ok/" + std::to_string(thread) + "/" + std::to_string(i), i);
      }
    }
  });
  auto unchanged = store.template Get<uint32_t>("failed");
  ASSERT_TRUE(unchanged.has_value());
  ASSERT_EQ(**unchanged, 7U);
  for (uint32_t thread : {1U, 3U}) {
    for (uint32_t i = 0; i < 80; ++i) {
      auto value = store.template Get<uint32_t>("ok/" + std::to_string(thread) + "/" + std::to_string(i));
      ASSERT_TRUE(value.has_value());
      ASSERT_EQ(**value, i);
    }
  }
}

STORE_TEST(NoOpRemovesDoNotLoseConcurrentInserts) {
  Store store;
  store.template Put<uint32_t>("prefix/sentinel", 42);
  testutil::Parallel(4, [&](uint32_t thread) {
    if (thread < 2) {
      for (uint32_t i = 0; i < 1000; ++i) {
        store.Remove("never-present");
        store.Remove("prefix");  // Existing path, but no value.
        store.Remove("");        // Existing root, but no value.
      }
    } else {
      for (uint32_t i = 0; i < 250; ++i) {
        auto key = "prefix/" + std::to_string(thread) + "/" + std::to_string(i);
        store.template Put<uint32_t>(key, i);
      }
    }
  });
  ASSERT_TRUE(!store.template Get<uint32_t>("prefix"));
  ASSERT_TRUE(!store.template Get<uint32_t>(""));
  auto sentinel = store.template Get<uint32_t>("prefix/sentinel");
  ASSERT_TRUE(sentinel.has_value());
  ASSERT_EQ(**sentinel, 42U);
  for (uint32_t thread : {2U, 3U}) {
    for (uint32_t i = 0; i < 250; ++i) {
      auto value = store.template Get<uint32_t>("prefix/" + std::to_string(thread) + "/" + std::to_string(i));
      ASSERT_TRUE(value.has_value());
      ASSERT_EQ(**value, i);
    }
  }
}

STORE_TEST(ConcurrentPrefixRemovalKeepsDescendantUpdates) {
  for (const auto &prefix : std::array<std::string, 2>{"", "p"}) {
    Store store;
    store.template Put<uint32_t>(prefix, 7);
    auto old_parent = store.template Get<uint32_t>(prefix);
    ASSERT_TRUE(old_parent.has_value());
    testutil::Parallel(3, [&](uint32_t thread) {
      if (thread == 0) {
        for (uint32_t i = 0; i < 400; ++i) {
          store.template Put<std::string>(prefix, "temporary-parent");
          store.Remove(prefix);
        }
      } else {
        for (uint32_t i = 0; i < 300; ++i) {
          auto key = prefix + "/child/" + std::to_string(thread) + "/" + std::to_string(i);
          store.template Put<Integer>(key, std::make_unique<uint32_t>(i));
        }
      }
    });
    ASSERT_EQ(**old_parent, 7U);
    ASSERT_TRUE(!store.template Get<uint32_t>(prefix));
    ASSERT_TRUE(!store.template Get<std::string>(prefix));
    for (uint32_t thread : {1U, 2U}) {
      for (uint32_t i = 0; i < 300; ++i) {
        auto key = prefix + "/child/" + std::to_string(thread) + "/" + std::to_string(i);
        auto child = store.template Get<Integer>(key);
        ASSERT_TRUE(child.has_value());
        ASSERT_NE((**child).get(), nullptr);
        ASSERT_EQ(***child, i);
      }
    }
  }
}

STORE_TEST(RetainedGuardDoesNotBlockWriters) {
  Store store;
  store.template Put<uint32_t>("x", 1);
  auto held = store.template Get<uint32_t>("x");
  ASSERT_TRUE(held.has_value());
  auto writer = std::async(std::launch::async, [&] {
    for (uint32_t i = 0; i < 100; ++i) {
      store.template Put<uint32_t>("x", i);
      store.Remove("x");
    }
  });
  const auto status = writer.wait_for(std::chrono::seconds(5));
  const auto old_value = **held;
  // Release before joining so an incorrect lock-holding guard can report a failure.
  held.reset();
  writer.get();
  ASSERT_EQ(status, std::future_status::ready);
  ASSERT_EQ(old_value, 1U);
  ASSERT_TRUE(!store.template Get<uint32_t>("x"));
}

#undef STORE_TEST

TEST(CasStoreTest, StaleWriterKeepsInterveningPrefixChanges) {
  CasTrieStore store;
  store.Put<uint32_t>("p", 1);
  store.Put<uint32_t>("p/keep", 2);
  store.Put<uint32_t>("p/delete", 3);
  std::promise<int> resume;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  auto slow = std::async(std::launch::async, [&] {
    store.Put<MoveBlocked>("p/new", MoveBlocked(resume.get_future(), &entered, 99));
  });
  const auto entered_status = entered_future.wait_for(std::chrono::seconds(5));
  if (entered_status != std::future_status::ready) {
    resume.set_value(0);
    slow.get();
    ASSERT_EQ(entered_status, std::future_status::ready);
  }
  auto other = std::async(std::launch::async, [&] {
    store.Remove("p");
    store.Put<std::string>("p/keep", "changed");
    store.Remove("p/delete");
    store.Put<std::unique_ptr<uint32_t>>("p/new/child", std::make_unique<uint32_t>(42));
  });
  const auto status = other.wait_for(std::chrono::seconds(5));
  resume.set_value(0);
  slow.get();
  other.get();
  ASSERT_EQ(status, std::future_status::ready);
  ASSERT_TRUE(!store.Get<uint32_t>("p"));
  ASSERT_TRUE(!store.Get<uint32_t>("p/delete"));
  auto kept = store.Get<std::string>("p/keep");
  auto parent = store.Get<MoveBlocked>("p/new");
  auto child = store.Get<std::unique_ptr<uint32_t>>("p/new/child");
  ASSERT_TRUE(kept && parent && child);
  ASSERT_EQ(**kept, "changed");
  ASSERT_NE((**parent).GetValue(), nullptr);
  ASSERT_EQ(*(**parent).GetValue(), 99U);
  ASSERT_NE((**child).get(), nullptr);
  ASSERT_EQ(***child, 42U);
}
}  // namespace trie

int main(int argc, char **argv) { return labtest::Run(argc, argv); }
