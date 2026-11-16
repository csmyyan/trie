#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "test.h"
#include "trie.h"
#include "trie_store.h"

namespace performance {
using Clock = std::chrono::steady_clock;
using Map = std::map<std::string, uint32_t>;
std::atomic<uint64_t> observed{0};

struct Sample {
  double nanoseconds;
  std::size_t operations;
};

auto Keys(uint32_t count) -> std::vector<std::string> {
  std::vector<std::string> keys;
  for (uint32_t i = 0; i < count; ++i) { keys.push_back("key/" + labtest::Pad(i, 8)); }
  return keys;
}

auto Populate(const std::vector<std::string> &keys) -> trie::Trie {
  trie::Trie tree;
  for (uint32_t i = 0; i < keys.size(); ++i) { tree = tree.Put<uint32_t>(keys[i], i + 1); }
  return tree;
}

template <class Work>
auto Time(std::size_t operations, Work work) -> Sample {
  const auto begin = Clock::now();
  const auto checksum = work();
  const auto end = Clock::now();
  observed.fetch_xor(checksum, std::memory_order_relaxed);
  return {std::chrono::duration<double, std::nano>(end - begin).count(), operations};
}

// Repeat identical workload batches until at least 20 ms of measured work (up to 64 batches).
// Setup, thread creation and the final teardown are excluded by each workload.
template <class Run>
auto StableSample(Run run) -> double {
  double elapsed = 0;
  std::size_t operations = 0;
  for (int batch = 0; batch < 64; ++batch) {
    const auto sample = run();
    ASSERT_TRUE(std::isfinite(sample.nanoseconds) && sample.nanoseconds > 0);
    ASSERT_TRUE(sample.operations > 0);
    elapsed += sample.nanoseconds;
    operations += sample.operations;
    if (elapsed >= 20'000'000) { break; }
  }
  return elapsed / static_cast<double>(operations);
}

auto Median(std::array<double, 5> samples) -> double {
  std::sort(samples.begin(), samples.end());
  return samples[2];
}

template <class Baseline, class Candidate>
void Compare(const std::string &name, Baseline baseline, Candidate candidate, double minimum_ratio) {
#ifndef NDEBUG
  throw std::runtime_error("Performance checks require an optimized Release build (NDEBUG).");
#endif
  baseline();  // Warm up both paths before collecting samples.
  candidate();
  std::array<double, 5> base{}, tested{}, ratio{};
  for (std::size_t round = 0; round < ratio.size(); ++round) {
    if (round % 2 == 0) {
      base[round] = StableSample(baseline);
      tested[round] = StableSample(candidate);
    } else {
      tested[round] = StableSample(candidate);
      base[round] = StableSample(baseline);
    }
    ratio[round] = base[round] / tested[round];
  }
  const auto relative = Median(ratio);
  std::cout << std::fixed << std::setprecision(3) << "[PERF] " << name
            << " baseline_ns/op=" << Median(base) << " candidate_ns/op=" << Median(tested)
            << " throughput_ratio=" << relative << " required>=" << minimum_ratio << '\n';
  if (relative < minimum_ratio) {
    throw std::runtime_error(name + ": relative throughput below requirement; rerun alone on an idle machine");
  }
}

TEST(TriePerformance, LookupRelativeToMap) {
  const auto keys = Keys(4096);
  auto tree = Populate(keys);
  Map baseline;
  std::vector<std::string> misses;
  for (uint32_t i = 0; i < keys.size(); ++i) {
    baseline[keys[i]] = i + 1;
    misses.push_back(keys[i] + "/missing");
  }
  struct Query { const std::string *key; uint32_t expected; };
  std::vector<Query> queries;
  for (uint32_t i = 0; i < 32768; ++i) {
    const auto slot = (i * 104729U + 17U) % keys.size();
    const bool miss = i % 8 == 0;
    queries.push_back({miss ? &misses[slot] : &keys[slot], miss ? 0U : static_cast<uint32_t>(slot + 1)});
  }
  auto map_lookup = [&] {
    return Time(queries.size(), [&] {
      uint64_t sum = 0;
      for (const auto &query : queries) {
        auto found = baseline.find(*query.key);
        const auto value = found == baseline.end() ? 0U : found->second;
        ASSERT_EQ(value, query.expected);
        sum += value;
      }
      return sum;
    });
  };
  auto trie_lookup = [&] {
    return Time(queries.size(), [&] {
      uint64_t sum = 0;
      for (const auto &query : queries) {
        const auto *found = tree.Get<uint32_t>(*query.key);
        const auto value = found == nullptr ? 0U : *found;
        ASSERT_EQ(value, query.expected);
        sum += value;
      }
      return sum;
    });
  };
  Compare("task1/lookup_vs_map", map_lookup, trie_lookup, 0.10);
}

auto UpdateTrie(const trie::Trie &initial, const std::vector<std::string> &keys,
                uint32_t steps) -> Sample {
  auto tree = initial;
  std::array<trie::Trie, 8> snapshots;
  return Time(steps, [&] {
    uint64_t sum = 0;
    for (uint32_t step = 0; step < steps; ++step) {
      snapshots[step % snapshots.size()] = tree;
      // Pair deletion with reinsertion, so both tree sizes do real deletions on every cycle.
      const auto &key = keys[((step / 2) * 37U) % keys.size()];
      if (step % 4 == 0) {
        tree = tree.Remove(key);
        ASSERT_EQ(tree.Get<uint32_t>(key), nullptr);
      } else {
        tree = tree.Put<uint32_t>(key, step + 1);
        const auto *value = tree.Get<uint32_t>(key);
        ASSERT_NE(value, nullptr);
        ASSERT_EQ(*value, step + 1);
        sum += *value;
      }
    }
    return sum;
  });
}

TEST(TriePerformance, CopyOnWriteRelativeToFullCopy) {
  const auto keys = Keys(4096);
  const auto tree = Populate(keys);
  auto initial = std::make_shared<Map>();
  for (uint32_t i = 0; i < keys.size(); ++i) { (*initial)[keys[i]] = i + 1; }
  constexpr uint32_t steps = 768;
  auto full_copy = [&] {
    std::shared_ptr<const Map> current = initial;
    std::array<std::shared_ptr<const Map>, 8> snapshots;
    return Time(steps, [&] {
      uint64_t sum = 0;
      for (uint32_t step = 0; step < steps; ++step) {
        snapshots[step % snapshots.size()] = current;
        auto next = std::make_shared<Map>(*current);
        const auto &key = keys[((step / 2) * 37U) % keys.size()];
        if (step % 4 == 0) {
          next->erase(key);
          ASSERT_EQ(next->find(key), next->end());
        } else {
          (*next)[key] = step + 1;
          ASSERT_EQ(next->at(key), step + 1);
          sum += next->at(key);
        }
        current = std::move(next);
      }
      return sum;
    });
  };
  Compare("task1/cow_vs_full_map_copy", full_copy, [&] { return UpdateTrie(tree, keys, steps); }, 3.0);
}

TEST(TriePerformance, UpdateCostWithLargerTree) {
  const auto small_keys = Keys(512);
  const auto large_keys = Keys(8192);
  const auto small = Populate(small_keys);
  const auto large = Populate(large_keys);
  // Fixed-width keys; 16 times as many keys should not cause 16 times the update cost.
  Compare("task1/8192_vs_512_keys", [&] { return UpdateTrie(small, small_keys, 4096); },
          [&] { return UpdateTrie(large, large_keys, 4096); }, 0.25);
}

// A deliberately coarse but correct TEST-ONLY baseline: one lock covers the entire operation.
// Both baseline and submitted Stores use the same submitted Trie and ValueGuard.
class SerialStore {
 public:
  template <class T>
  auto Get(std::string_view key) -> std::optional<trie::ValueGuard<T>> {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto *value = root_.Get<T>(key);
    if (!value) { return std::nullopt; }
    return trie::ValueGuard<T>(root_, *value);
  }

  template <class T>
  void Put(std::string_view key, T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    root_ = root_.Put<T>(key, std::move(value));
  }

  void Remove(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    root_ = root_.Remove(key);
  }

 private:
  std::mutex mutex_;
  trie::Trie root_;
};

struct Workload {
  uint32_t reads;
  uint32_t writes;
  uint32_t keys;
  uint32_t steps;
};

template <class Store>
auto RunStore(uint32_t threads, Workload load) -> Sample {
  const auto keys = Keys(load.keys);
  Store store;
  for (uint32_t i = 0; i < keys.size(); ++i) { store.template Put<uint32_t>(keys[i], i + 1); }
  std::promise<void> start;
  auto gate = start.get_future().share();
  std::mutex ready_lock;
  std::condition_variable ready_condition;
  uint32_t ready = 0;
  std::vector<std::future<uint64_t>> workers;
  try {
    for (uint32_t thread = 0; thread < threads; ++thread) {
      workers.push_back(std::async(std::launch::async, [&, thread, gate] {
        {
          std::lock_guard<std::mutex> lock(ready_lock);
          ++ready;
          ready_condition.notify_one();
        }
        gate.wait();
        uint64_t checksum = 0;
        for (uint32_t step = 0; step < load.steps; ++step) {
          const auto slot = (step * 104729U + thread * 997U) % keys.size();
          const uint32_t value = static_cast<uint32_t>(slot + 1);
          const auto operation = (step * 17U + thread * 13U) % 100;
          if (operation < load.reads) {
            auto guard = store.template Get<uint32_t>(keys[slot]);
            if (load.reads == 100) { ASSERT_TRUE(guard.has_value()); }
            if (guard) {
              ASSERT_EQ(**guard, value);
              checksum += **guard;
            }
          } else if (operation < load.reads + load.writes) {
            store.template Put<uint32_t>(keys[slot], value);
            checksum += value;
          } else {
            store.Remove(keys[slot]);
          }
        }
        return checksum;
      }));
    }
  } catch (...) {
    start.set_value();
    throw;
  }
  {
    std::unique_lock<std::mutex> lock(ready_lock);
    ready_condition.wait(lock, [&] { return ready == threads; });
  }
  const auto begin = Clock::now();
  start.set_value();
  uint64_t checksum = 0;
  std::exception_ptr failure;
  for (auto &worker : workers) {
    try {
      checksum += worker.get();
    } catch (...) {
      if (!failure) { failure = std::current_exception(); }
    }
  }
  const auto end = Clock::now();
  if (failure) { std::rethrow_exception(failure); }
  ASSERT_TRUE(checksum > 0);
  observed.fetch_xor(checksum, std::memory_order_relaxed);
  return {std::chrono::duration<double, std::nano>(end - begin).count(),
          static_cast<std::size_t>(threads) * load.steps};
}

template <class Store>
void StoreComparison(const std::string &name, uint32_t threads, Workload load, double minimum) {
  Compare(name + "/threads=" + std::to_string(threads),
          [&] { return RunStore<SerialStore>(threads, load); },
          [&] { return RunStore<Store>(threads, load); }, minimum);
}

#define STORE_PERFORMANCE_TEST(test_name)                                                \
  template <class Store> void Check##test_name(const std::string &name);                  \
  TEST(StorePerformance, test_name) { Check##test_name<trie::TrieStore>("task2a/" #test_name); } \
  TEST(CasPerformance, test_name) { Check##test_name<trie::CasTrieStore>("task2b/" #test_name); } \
  template <class Store> void Check##test_name(const std::string &name)

STORE_PERFORMANCE_TEST(ReadOnlyRelativeToSerial) {
  for (uint32_t threads : {1U, 4U}) {
    StoreComparison<Store>(name, threads, {100, 0, 512, 8192}, 0.25);
  }
}

STORE_PERFORMANCE_TEST(MixedRelativeToSerial) {
  StoreComparison<Store>(name, 4, {80, 15, 512, 4096}, 0.25);
}

STORE_PERFORMANCE_TEST(HotWritesRelativeToSerial) {
  StoreComparison<Store>(name, 4, {40, 45, 16, 2048}, 0.125);
}

#undef STORE_PERFORMANCE_TEST
}  // namespace performance

int main(int argc, char **argv) { return labtest::Run(argc, argv); }
