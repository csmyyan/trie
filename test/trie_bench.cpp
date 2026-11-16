#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "trie.h"

namespace {
using Clock = std::chrono::steady_clock;
enum class Mode { Snapshot, CountedLookup, BorrowedLookup };
struct Result {
  double nanoseconds;
  uint64_t checksum;
};

auto Build(const std::vector<std::string> &keys) -> trie::Trie {
  trie::Trie tree;
  for (uint32_t i = 0; i < keys.size(); ++i) {
    tree = tree.Put<uint32_t>(keys[i], i);
  }
  return tree;
}

// 仅用于测量逐节点复制 shared_ptr 的开销；Trie 的正常查询统一使用 Get。
auto CountedLookup(const trie::Trie &tree, std::string_view key) -> const uint32_t * {
  auto current = tree.GetRoot();
  if (current == nullptr) {
    return nullptr;
  }
  for (char c : key) {
    auto it = current->children_.find(c);
    if (it == current->children_.end()) {
      return nullptr;
    }
    current = it->second;
  }
  auto *typed = dynamic_cast<const trie::TrieNodeWithValue<uint32_t> *>(current.get());
  const auto *value = typed == nullptr ? nullptr : typed->value_.get();
  return value;  // tree 仍持有根，借用结果继续有效。
}

auto Parallel(const trie::Trie &base, const std::vector<std::string> &keys, Mode mode, uint64_t operations,
              uint64_t threads) -> Result {
  std::promise<void> start;
  auto ready = start.get_future().share();
  std::vector<std::future<uint64_t>> workers;
  for (uint64_t tid = 0; tid < threads; ++tid) {
    workers.push_back(std::async(std::launch::async, [&, tid, ready] {
      ready.wait();
      uint64_t checksum = 0;
      for (uint64_t i = 0; i < operations; ++i) {
        if (mode == Mode::Snapshot) {
          trie::Trie snapshot = base;
          checksum += snapshot.GetRoot() != nullptr;
        } else {
          const auto &key = keys[(i * 977 + tid * 37) % keys.size()];
          const auto *value = mode == Mode::CountedLookup ? CountedLookup(base, key) : base.Get<uint32_t>(key);
          if (value == nullptr) {
            throw std::runtime_error("lookup lost a key");
          }
          checksum += *value;
        }
      }
      return checksum;
    }));
  }
  const auto begin = Clock::now();
  start.set_value();
  uint64_t checksum = 0;
  for (auto &worker : workers) {
    checksum += worker.get();
  }
  return {std::chrono::duration<double, std::nano>(Clock::now() - begin).count(), checksum};
}

auto Mixed(const trie::Trie &base, const std::vector<std::string> &keys, uint64_t operations) -> Result {
  uint64_t checksum = 0;
  const auto begin = Clock::now();
  {
    trie::Trie current = base;
    std::array<trie::Trie, 8> snapshots;
    for (uint64_t i = 0; i < operations; ++i) {
      if (i % 8 == 0) {
        snapshots[(i / 8) % snapshots.size()] = current;
      }
      const auto &key = keys[(i * 977) % keys.size()];
      if (i % 5 == 0) {
        current = current.Remove(key);
      } else {
        current = current.Put<uint32_t>(key, static_cast<uint32_t>(i));
      }
    }
    for (const auto &key : keys) {
      const auto *value = current.Get<uint32_t>(key);
      checksum += value == nullptr ? 0 : static_cast<uint64_t>(*value) + 1;
    }
  }  // 包含当前版本和历史快照的销毁时间。
  return {std::chrono::duration<double, std::nano>(Clock::now() - begin).count(), checksum};
}

void Print(const char *workload, int round, uint64_t threads, uint64_t operations,
           const Result &result) {
  std::cout << workload << ',' << round << ',' << threads << ',' << operations << ','
            << result.nanoseconds / static_cast<double>(operations) << ','
            << static_cast<double>(operations) * 1e9 / result.nanoseconds << ',' << result.checksum << '\n';
}

auto Number(const char *argument) -> uint64_t {
  std::size_t used = 0;
  const std::string text(argument);
  if (text.empty() || text.front() == '-') {
    throw std::invalid_argument("arguments must be positive integers");
  }
  auto value = std::stoull(text, &used);
  if (used != text.size() || value == 0) {
    throw std::invalid_argument("arguments must be positive integers");
  }
  return value;
}
}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc > 4) {
      throw std::invalid_argument("usage: trie_bench [operations-per-thread=100000] [threads=4] [keys=1024]");
    }
    const auto operations = argc > 1 ? Number(argv[1]) : 100000;
    const auto threads = argc > 2 ? Number(argv[2]) : 4;
    const auto count = argc > 3 ? Number(argv[3]) : 1024;
    if (operations > 100000000 || threads > 64 || count > 1000000) {
      throw std::invalid_argument("limits: operations <= 100000000, threads <= 64, keys <= 1000000");
    }
    std::vector<std::string> keys;
    for (uint64_t i = 0; i < count; ++i) {
      const auto suffix = std::to_string(i);
      keys.push_back("key/" + std::string(7 - suffix.size(), '0') + suffix);
    }
    const auto tree = Build(keys);

    // 在计时区间外计算期望结果，避免测试只检查运行过程中是否崩溃。
    uint64_t expected_read = 0;
    for (uint64_t tid = 0; tid < threads; ++tid) {
      for (uint64_t i = 0; i < operations; ++i) {
        expected_read += (i * 977 + tid * 37) % keys.size();
      }
    }
    const auto writes = operations / 10 + 1;
    std::map<std::string, uint32_t> expected_values;
    for (uint32_t i = 0; i < keys.size(); ++i) {
      expected_values.emplace(keys[i], i);
    }
    for (uint64_t i = 0; i < writes; ++i) {
      const auto &key = keys[(i * 977) % keys.size()];
      if (i % 5 == 0) {
        expected_values.erase(key);
      } else {
        expected_values[key] = static_cast<uint32_t>(i);
      }
    }
    uint64_t expected_write = 0;
    for (const auto &entry : expected_values) {
      expected_write += static_cast<uint64_t>(entry.second) + 1;
    }

    std::cout << "# sizeof_bytes: trie=" << sizeof(tree) << ",node=" << sizeof(trie::TrieNode)
              << ",edge=" << sizeof(trie::TrieNode::Children::mapped_type) << '\n';
    std::cout << "workload,round,threads,operations,ns_per_op,ops_per_second,checksum\n";
    std::cout << std::fixed << std::setprecision(2);
    for (int round = 1; round <= 3; ++round) {
      auto snapshot = Parallel(tree, keys, Mode::Snapshot, operations, threads);
      Result counted{}, borrowed{};
      if (round % 2 == 1) {
        counted = Parallel(tree, keys, Mode::CountedLookup, operations, threads);
        borrowed = Parallel(tree, keys, Mode::BorrowedLookup, operations, threads);
      } else {
        borrowed = Parallel(tree, keys, Mode::BorrowedLookup, operations, threads);
        counted = Parallel(tree, keys, Mode::CountedLookup, operations, threads);
      }
      if (snapshot.checksum != operations * threads || counted.checksum != expected_read ||
          borrowed.checksum != expected_read) {
        throw std::runtime_error("read checksum mismatch");
      }
      Print("snapshot_copy", round, threads, operations * threads, snapshot);
      Print("lookup_counted", round, threads, operations * threads, counted);
      Print("lookup_borrowed", round, threads, operations * threads, borrowed);
      auto mixed = Mixed(tree, keys, writes);
      if (mixed.checksum != expected_write) {
        throw std::runtime_error("write checksum mismatch");
      }
      Print("mixed_cow", round, 1, writes, mixed);
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
