#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "test.h"
#include "trie.h"

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
// Initial tree construction and final teardown are excluded from each measured batch.
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

auto UpdateTrie(const trie::Trie &initial, const std::vector<std::string> &keys,
                uint32_t steps) -> Sample {
  auto tree = initial;
  std::array<trie::Trie, 8> snapshots;
  return Time(steps, [&] {
    uint64_t sum = 0;
    for (uint32_t step = 0; step < steps; ++step) {
      snapshots[step % snapshots.size()] = tree;
      // Pair deletion with reinsertion so each deletion removes an existing key.
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
  Compare("cow_vs_full_map_copy", full_copy, [&] { return UpdateTrie(tree, keys, steps); }, 16.0);
}

}  // namespace performance

int main(int argc, char **argv) { return labtest::Run(argc, argv); }
