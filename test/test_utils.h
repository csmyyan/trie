#pragma once

#include <cstdint>
#include <exception>
#include <future>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "test.h"
#include "trie.h"

// Test-only helpers: the oracle is an ordinary map, independent of Trie algorithms.
namespace testutil {
using Value = std::variant<uint32_t, std::string>;
using Model = std::map<std::string, Value>;

inline auto Keys() -> std::vector<std::string> {
  std::vector<std::string> keys{"", "p", "prefix", "prefix/", std::string(512, 'x')};
  for (int byte = 0; byte < 256; ++byte) {
    keys.push_back("prefix/" + std::string(1, static_cast<char>(byte)) + std::string("\0end", 4));
  }
  for (int length = 1; length <= 24; ++length) {
    keys.emplace_back(length, 'a');
  }
  return keys;
}

inline void Check(const trie::Trie &tree, const Model &model, const std::vector<std::string> &keys) {
  for (const auto &key : keys) {
    auto expected = model.find(key);
    const auto *integer = tree.Get<uint32_t>(key);
    const auto *text = tree.Get<std::string>(key);
    if (expected == model.end()) {
      ASSERT_EQ(integer, nullptr);
      ASSERT_EQ(text, nullptr);
    } else if (std::holds_alternative<uint32_t>(expected->second)) {
      ASSERT_NE(integer, nullptr);
      ASSERT_EQ(*integer, std::get<uint32_t>(expected->second));
      ASSERT_EQ(text, nullptr);
    } else {
      ASSERT_NE(text, nullptr);
      ASSERT_EQ(*text, std::get<std::string>(expected->second));
      ASSERT_EQ(integer, nullptr);
    }
  }
}

// The exact set of live node paths is the set of prefixes of the remaining keys.
// This detects both over-pruning and leftover empty nodes, including a non-null empty root.
inline void CheckShape(const trie::Trie &tree, const Model &model) {
  std::set<std::string> expected;
  for (const auto &entry : model) {
    for (std::size_t length = 0; length <= entry.first.size(); ++length) {
      expected.insert(entry.first.substr(0, length));
    }
  }
  std::set<std::string> actual;
  std::vector<std::pair<std::shared_ptr<const trie::TrieNode>, std::string>> pending;
  if (tree.GetRoot()) { pending.emplace_back(tree.GetRoot(), ""); }
  while (!pending.empty()) {
    auto entry = std::move(pending.back());
    pending.pop_back();
    ASSERT_NE(entry.first, nullptr);
    actual.insert(entry.second);
    ASSERT_TRUE(actual.size() <= expected.size());
    ASSERT_EQ(entry.first->is_value_node_, model.count(entry.second) != 0);
    for (const auto &child : entry.first->children_) {
      pending.emplace_back(child.second, entry.second + child.first);
    }
  }
  ASSERT_EQ(actual, expected);
}

template <class Store>
void CheckStore(Store &store, const Model &model, const std::vector<std::string> &keys) {
  for (const auto &key : keys) {
    auto expected = model.find(key);
    auto integer = store.template Get<uint32_t>(key);
    auto text = store.template Get<std::string>(key);
    if (expected == model.end()) {
      ASSERT_TRUE(!integer && !text);
    } else if (std::holds_alternative<uint32_t>(expected->second)) {
      ASSERT_TRUE(integer.has_value());
      ASSERT_EQ(**integer, std::get<uint32_t>(expected->second));
      ASSERT_TRUE(!text);
    } else {
      ASSERT_TRUE(text.has_value());
      ASSERT_EQ(**text, std::get<std::string>(expected->second));
      ASSERT_TRUE(!integer);
    }
  }
}

// Propagate worker assertions after joining every worker. Launch failures also release the gate.
template <class Function>
void Parallel(uint32_t count, Function work) {
  std::promise<void> start;
  auto ready = start.get_future().share();
  std::vector<std::future<void>> workers;
  try {
    for (uint32_t thread = 0; thread < count; ++thread) {
      workers.push_back(std::async(std::launch::async, [&, ready, thread] {
        ready.wait();
        work(thread);
      }));
    }
  } catch (...) {
    start.set_value();
    throw;
  }
  start.set_value();
  std::exception_ptr failure;
  for (auto &worker : workers) {
    try {
      worker.get();
    } catch (...) {
      if (!failure) {
        failure = std::current_exception();
      }
    }
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
}
}  // namespace testutil
