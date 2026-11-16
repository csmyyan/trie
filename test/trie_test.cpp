// Adapted from CMU BusTub v20231227-2023fall; see LICENSE.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/move_blocked.h"
#include "common/tracked_value.h"
#include "test.h"
#include "test_utils.h"
#include "trie.h"

namespace trie {
using Integer = std::unique_ptr<uint32_t>;

TEST(StarterSupport, EmptyTrieHasNoRoot) {
  ASSERT_EQ(Trie().GetRoot(), nullptr);
}

TEST(StarterSupport, CloneSharesChildren) {
  auto child = std::make_shared<TrieNode>();
  auto original = std::make_shared<TrieNode>();
  original->children_['a'] = child;
  auto clone = original->Clone();
  ASSERT_NE(clone, original);
  ASSERT_EQ(clone->children_.at('a'), child);
  clone->children_.erase('a');
  ASSERT_EQ(original->children_.size(), 1U);
}

TEST(StarterSupport, CloneSharesNonCopyableValue) {
  auto original = std::make_shared<TrieNodeWithValue<Integer>>(
      std::make_shared<Integer>(std::make_unique<uint32_t>(42)));
  auto clone = original->Clone();
  auto *typed = dynamic_cast<const TrieNodeWithValue<Integer> *>(clone.get());
  ASSERT_NE(typed, nullptr);
  ASSERT_TRUE(typed->is_value_node_);
  ASSERT_EQ(typed->value_, original->value_);
  original.reset();
  ASSERT_EQ(**typed->value_, 42U);
}


TEST(TrieTest, ConstructorTest) { ASSERT_EQ(Trie().GetRoot(), nullptr); }

TEST(TrieTest, BasicPutTest) {
  auto trie = Trie();
  trie = trie.Put<uint32_t>("test-int", 233);
  trie = trie.Put<uint64_t>("test-int2", 23333333);
  trie = trie.Put<std::string>("test-string", "test");
  trie = trie.Put<std::string>("", "empty-key");
}

TEST(TrieTest, TrieStructureCheck) {
  auto trie = Trie();
  // Put something
  trie = trie.Put<uint32_t>("test", 233);
  ASSERT_EQ(*trie.Get<uint32_t>("test"), 233);
  // Ensure the trie is the same representation of the writeup
  // (Some students were using '\0' as the terminator in previous semesters)
  auto root = trie.GetRoot();
  ASSERT_EQ(root->children_.size(), 1);
  ASSERT_EQ(root->children_.at('t')->children_.size(), 1);
  ASSERT_EQ(root->children_.at('t')->children_.at('e')->children_.size(), 1);
  ASSERT_EQ(root->children_.at('t')->children_.at('e')->children_.at('s')->children_.size(), 1);
  ASSERT_EQ(root->children_.at('t')->children_.at('e')->children_.at('s')->children_.at('t')->children_.size(), 0);
  ASSERT_TRUE(root->children_.at('t')->children_.at('e')->children_.at('s')->children_.at('t')->is_value_node_);
}

TEST(TrieTest, BasicPutGetTest) {
  auto trie = Trie();
  // Put something
  trie = trie.Put<uint32_t>("test", 233);
  ASSERT_EQ(*trie.Get<uint32_t>("test"), 233);
  // Put something else
  trie = trie.Put<uint32_t>("test", 23333333);
  ASSERT_EQ(*trie.Get<uint32_t>("test"), 23333333);
  // Overwrite with another type
  trie = trie.Put<std::string>("test", "23333333");
  ASSERT_EQ(*trie.Get<std::string>("test"), "23333333");
  // Get something that doesn't exist
  ASSERT_EQ(trie.Get<std::string>("test-2333"), nullptr);
  // Put something at root
  trie = trie.Put<std::string>("", "empty-key");
  ASSERT_EQ(*trie.Get<std::string>(""), "empty-key");
}

TEST(TrieTest, PutGetOnePath) {
  auto trie = Trie();
  trie = trie.Put<uint32_t>("111", 111);
  trie = trie.Put<uint32_t>("11", 11);
  trie = trie.Put<uint32_t>("1111", 1111);
  trie = trie.Put<uint32_t>("11", 22);
  ASSERT_EQ(*trie.Get<uint32_t>("11"), 22);
  ASSERT_EQ(*trie.Get<uint32_t>("111"), 111);
  ASSERT_EQ(*trie.Get<uint32_t>("1111"), 1111);
}

TEST(TrieTest, BasicRemoveTest1) {
  auto trie = Trie();
  // Put something
  trie = trie.Put<uint32_t>("test", 2333);
  ASSERT_EQ(*trie.Get<uint32_t>("test"), 2333);
  trie = trie.Put<uint32_t>("te", 23);
  ASSERT_EQ(*trie.Get<uint32_t>("te"), 23);
  trie = trie.Put<uint32_t>("tes", 233);
  ASSERT_EQ(*trie.Get<uint32_t>("tes"), 233);
  // Delete something
  trie = trie.Remove("test");
  trie = trie.Remove("tes");
  trie = trie.Remove("te");

  ASSERT_EQ(trie.Get<uint32_t>("te"), nullptr);
  ASSERT_EQ(trie.Get<uint32_t>("tes"), nullptr);
  ASSERT_EQ(trie.Get<uint32_t>("test"), nullptr);
}

TEST(TrieTest, BasicRemoveTest2) {
  auto trie = Trie();
  // Put something
  trie = trie.Put<uint32_t>("test", 2333);
  ASSERT_EQ(*trie.Get<uint32_t>("test"), 2333);
  trie = trie.Put<uint32_t>("te", 23);
  ASSERT_EQ(*trie.Get<uint32_t>("te"), 23);
  trie = trie.Put<uint32_t>("tes", 233);
  ASSERT_EQ(*trie.Get<uint32_t>("tes"), 233);
  trie = trie.Put<uint32_t>("", 123);
  ASSERT_EQ(*trie.Get<uint32_t>(""), 123);
  // Delete something
  trie = trie.Remove("");
  trie = trie.Remove("te");
  trie = trie.Remove("tes");
  trie = trie.Remove("test");

  ASSERT_EQ(trie.Get<uint32_t>(""), nullptr);
  ASSERT_EQ(trie.Get<uint32_t>("te"), nullptr);
  ASSERT_EQ(trie.Get<uint32_t>("tes"), nullptr);
  ASSERT_EQ(trie.Get<uint32_t>("test"), nullptr);
}

TEST(TrieTest, RemoveFreeTest) {
  auto trie = Trie();
  trie = trie.Put<uint32_t>("test", 2333);
  trie = trie.Put<uint32_t>("te", 23);
  trie = trie.Put<uint32_t>("tes", 233);
  trie = trie.Remove("tes");
  trie = trie.Remove("test");
  ASSERT_EQ(trie.GetRoot()->children_.at('t')->children_.at('e')->children_.size(), 0);
  trie = trie.Remove("te");
  ASSERT_EQ(trie.GetRoot(), nullptr);
}

TEST(TrieTest, MismatchTypeTest) {
  auto trie = Trie();
  // Put something
  trie = trie.Put<uint32_t>("test", 2333);
  ASSERT_EQ(trie.Get<std::string>("test"), nullptr);
}

TEST(TrieTest, CopyOnWriteTest1) {
  auto empty_trie = Trie();
  // Put something
  auto trie1 = empty_trie.Put<uint32_t>("test", 2333);
  auto trie2 = trie1.Put<uint32_t>("te", 23);
  auto trie3 = trie2.Put<uint32_t>("tes", 233);

  // Delete something
  auto trie4 = trie3.Remove("te");
  auto trie5 = trie3.Remove("tes");
  auto trie6 = trie3.Remove("test");

  // Check each snapshot
  ASSERT_EQ(*trie3.Get<uint32_t>("te"), 23);
  ASSERT_EQ(*trie3.Get<uint32_t>("tes"), 233);
  ASSERT_EQ(*trie3.Get<uint32_t>("test"), 2333);

  ASSERT_EQ(trie4.Get<uint32_t>("te"), nullptr);
  ASSERT_EQ(*trie4.Get<uint32_t>("tes"), 233);
  ASSERT_EQ(*trie4.Get<uint32_t>("test"), 2333);

  ASSERT_EQ(*trie5.Get<uint32_t>("te"), 23);
  ASSERT_EQ(trie5.Get<uint32_t>("tes"), nullptr);
  ASSERT_EQ(*trie5.Get<uint32_t>("test"), 2333);

  ASSERT_EQ(*trie6.Get<uint32_t>("te"), 23);
  ASSERT_EQ(*trie6.Get<uint32_t>("tes"), 233);
  ASSERT_EQ(trie6.Get<uint32_t>("test"), nullptr);
}

TEST(TrieTest, CopyOnWriteTest2) {
  auto empty_trie = Trie();
  // Put something
  auto trie1 = empty_trie.Put<uint32_t>("test", 2333);
  auto trie2 = trie1.Put<uint32_t>("te", 23);
  auto trie3 = trie2.Put<uint32_t>("tes", 233);

  // Override something
  auto trie4 = trie3.Put<std::string>("te", "23");
  auto trie5 = trie3.Put<std::string>("tes", "233");
  auto trie6 = trie3.Put<std::string>("test", "2333");

  // Check each snapshot
  ASSERT_EQ(*trie3.Get<uint32_t>("te"), 23);
  ASSERT_EQ(*trie3.Get<uint32_t>("tes"), 233);
  ASSERT_EQ(*trie3.Get<uint32_t>("test"), 2333);

  ASSERT_EQ(*trie4.Get<std::string>("te"), "23");
  ASSERT_EQ(*trie4.Get<uint32_t>("tes"), 233);
  ASSERT_EQ(*trie4.Get<uint32_t>("test"), 2333);

  ASSERT_EQ(*trie5.Get<uint32_t>("te"), 23);
  ASSERT_EQ(*trie5.Get<std::string>("tes"), "233");
  ASSERT_EQ(*trie5.Get<uint32_t>("test"), 2333);

  ASSERT_EQ(*trie6.Get<uint32_t>("te"), 23);
  ASSERT_EQ(*trie6.Get<uint32_t>("tes"), 233);
  ASSERT_EQ(*trie6.Get<std::string>("test"), "2333");
}

TEST(TrieTest, CopyOnWriteTest3) {
  auto empty_trie = Trie();
  // Put something
  auto trie1 = empty_trie.Put<uint32_t>("test", 2333);
  auto trie2 = trie1.Put<uint32_t>("te", 23);
  auto trie3 = trie2.Put<uint32_t>("", 233);

  // Override something
  auto trie4 = trie3.Put<std::string>("te", "23");
  auto trie5 = trie3.Put<std::string>("", "233");
  auto trie6 = trie3.Put<std::string>("test", "2333");

  // Check each snapshot
  ASSERT_EQ(*trie3.Get<uint32_t>("te"), 23);
  ASSERT_EQ(*trie3.Get<uint32_t>(""), 233);
  ASSERT_EQ(*trie3.Get<uint32_t>("test"), 2333);

  ASSERT_EQ(*trie4.Get<std::string>("te"), "23");
  ASSERT_EQ(*trie4.Get<uint32_t>(""), 233);
  ASSERT_EQ(*trie4.Get<uint32_t>("test"), 2333);

  ASSERT_EQ(*trie5.Get<uint32_t>("te"), 23);
  ASSERT_EQ(*trie5.Get<std::string>(""), "233");
  ASSERT_EQ(*trie5.Get<uint32_t>("test"), 2333);

  ASSERT_EQ(*trie6.Get<uint32_t>("te"), 23);
  ASSERT_EQ(*trie6.Get<uint32_t>(""), 233);
  ASSERT_EQ(*trie6.Get<std::string>("test"), "2333");
}

TEST(TrieTest, MixedTest) {
  auto trie = Trie();
  for (uint32_t i = 0; i < 23333; i++) {
    std::string key = labtest::Pad(i, 5);
    std::string value = "value-" + labtest::Pad(i, 8);
    trie = trie.Put<std::string>(key, value);
  }
  auto trie_full = trie;
  for (uint32_t i = 0; i < 23333; i += 2) {
    std::string key = labtest::Pad(i, 5);
    std::string value = "new-value-" + labtest::Pad(i, 8);
    trie = trie.Put<std::string>(key, value);
  }
  auto trie_override = trie;
  for (uint32_t i = 0; i < 23333; i += 3) {
    std::string key = labtest::Pad(i, 5);
    trie = trie.Remove(key);
  }
  auto trie_final = trie;

  // verify trie_full
  for (uint32_t i = 0; i < 23333; i++) {
    std::string key = labtest::Pad(i, 5);
    std::string value = "value-" + labtest::Pad(i, 8);
    ASSERT_EQ(*trie_full.Get<std::string>(key), value);
  }

  // verify trie_override
  for (uint32_t i = 0; i < 23333; i++) {
    std::string key = labtest::Pad(i, 5);
    if (i % 2 == 0) {
      std::string value = "new-value-" + labtest::Pad(i, 8);
      ASSERT_EQ(*trie_override.Get<std::string>(key), value);
    } else {
      std::string value = "value-" + labtest::Pad(i, 8);
      ASSERT_EQ(*trie_override.Get<std::string>(key), value);
    }
  }

  // verify final trie
  for (uint32_t i = 0; i < 23333; i++) {
    std::string key = labtest::Pad(i, 5);
    if (i % 3 == 0) {
      ASSERT_EQ(trie_final.Get<std::string>(key), nullptr);
    } else if (i % 2 == 0) {
      std::string value = "new-value-" + labtest::Pad(i, 8);
      ASSERT_EQ(*trie_final.Get<std::string>(key), value);
    } else {
      std::string value = "value-" + labtest::Pad(i, 8);
      ASSERT_EQ(*trie_final.Get<std::string>(key), value);
    }
  }
}

TEST(TrieTest, PointerStability) {
  auto trie = Trie();
  trie = trie.Put<uint32_t>("test", 2333);
  auto *ptr_before = trie.Get<uint32_t>("test");
  ASSERT_NE(ptr_before, nullptr);
  trie = trie.Put<uint32_t>("tes", 233);
  trie = trie.Put<uint32_t>("te", 23);
  auto *ptr_after = trie.Get<uint32_t>("test");
  ASSERT_EQ(ptr_before, ptr_after);
}

TEST(TrieTest, NonCopyableTest) {
  auto trie = Trie();
  trie = trie.Put<Integer>("tes", std::make_unique<uint32_t>(233));
  trie = trie.Put<Integer>("te", std::make_unique<uint32_t>(23));
  trie = trie.Put<Integer>("test", std::make_unique<uint32_t>(2333));
  ASSERT_EQ(**trie.Get<Integer>("te"), 23);
  ASSERT_EQ(**trie.Get<Integer>("tes"), 233);
  ASSERT_EQ(**trie.Get<Integer>("test"), 2333);
  trie = trie.Remove("te");
  trie = trie.Remove("tes");
  trie = trie.Remove("test");
  ASSERT_EQ(trie.Get<Integer>("te"), nullptr);
  ASSERT_EQ(trie.Get<Integer>("tes"), nullptr);
  ASSERT_EQ(trie.Get<Integer>("test"), nullptr);
}

TEST(TrieTest, MissingKeyTest) {
  Trie empty;
  ASSERT_EQ(empty.Get<uint32_t>(""), nullptr);
  ASSERT_EQ(empty.Get<uint32_t>("missing"), nullptr);
  ASSERT_EQ(empty.Remove("missing").GetRoot(), nullptr);
  auto trie = empty.Put<uint32_t>("abc", 1);
  ASSERT_EQ(trie.Get<uint32_t>("ab"), nullptr);
  ASSERT_EQ(trie.Remove("ab").GetRoot(), trie.GetRoot());
  ASSERT_EQ(trie.Remove("absent").GetRoot(), trie.GetRoot());
}

TEST(TrieTest, SubtreeSharingTest) {
  auto old = Trie().Put<uint32_t>("ab", 1).Put<uint32_t>("ac", 2).Put<uint32_t>("z", 3);
  auto updated = old.Put<uint32_t>("ab", 4);
  auto removed = old.Remove("ab");
  for (const auto &trie : {updated, removed}) {
    ASSERT_NE(trie.GetRoot(), old.GetRoot());
    ASSERT_EQ(trie.GetRoot()->children_.at('z'), old.GetRoot()->children_.at('z'));
    ASSERT_EQ(trie.GetRoot()->children_.at('a')->children_.at('c'),
              old.GetRoot()->children_.at('a')->children_.at('c'));
  }
  ASSERT_EQ(*old.Get<uint32_t>("ab"), 1U);
}

TEST(TrieTest, ValueSurvivesClonedPrefix) {
  std::atomic<int> destroyed{0};
  auto original = Trie().Put<TrackedPtr>("a", std::make_unique<TrackedValue>(destroyed, 7));
  auto updated = original.Put<uint32_t>("ab", 8);
  const auto *value = original.Get<TrackedPtr>("a");
  ASSERT_NE(value, nullptr);
  ASSERT_EQ(value, updated.Get<TrackedPtr>("a"));
  original = Trie();
  ASSERT_EQ(destroyed.load(), 0);
  ASSERT_EQ((*value)->value_, 7U);
  updated = updated.Remove("a");
  ASSERT_EQ(destroyed.load(), 1);
  ASSERT_EQ(*updated.Get<uint32_t>("ab"), 8U);
}

TEST(TrieTest, HistoricalValuesAreFreedExactlyOnce) {
  std::atomic<int> destroyed{0};
  Trie current;
  std::vector<Trie> versions;
  for (uint32_t i = 0; i < 40; ++i) {
    current = current.Put<TrackedPtr>("key", std::make_unique<TrackedValue>(destroyed, i));
    versions.push_back(current);
  }
  current = Trie();
  ASSERT_EQ(destroyed.load(), 0);
  for (uint32_t i = 0; i < versions.size(); ++i) {
    ASSERT_EQ((*versions[i].Get<TrackedPtr>("key"))->value_, i);
  }
  versions.clear();
  ASSERT_EQ(destroyed.load(), 40);
}

TEST(TrieTest, LookupHandlesEmptyAndBinaryKeys) {
  const std::string binary_key("a\0b", 3);
  auto tree = Trie().Put<uint32_t>("", 1).Put<uint32_t>("abc", 2).Put<uint32_t>(binary_key, 3);
  ASSERT_EQ(*tree.Get<uint32_t>(""), 1U);
  ASSERT_EQ(*tree.Get<uint32_t>("abc"), 2U);
  ASSERT_EQ(*tree.Get<uint32_t>(binary_key), 3U);
  ASSERT_EQ(tree.Get<uint32_t>("ab"), nullptr);
  ASSERT_EQ(tree.Get<std::string>(binary_key), nullptr);
  auto updated = tree.Remove(binary_key);
  ASSERT_EQ(updated.Get<uint32_t>(binary_key), nullptr);
  ASSERT_EQ(*tree.Get<uint32_t>(binary_key), 3U);
  ASSERT_EQ(*updated.Get<uint32_t>("abc"), 2U);
}


TEST(TrieTest, GetDecisionMatrix) {
  const Trie empty;
  for (const auto *key : {"", "a", "abc"}) {
    ASSERT_EQ(empty.Get<uint32_t>(key), nullptr);
    ASSERT_EQ(empty.Get<std::string>(key), nullptr);
  }
  auto tree = Trie().Put<uint32_t>("", 10).Put<uint32_t>("abc", 20)
                   .Put<std::string>("abde", "text").Put<Integer>("z", std::make_unique<uint32_t>(30));
  const auto root = tree.GetRoot();
  // Missing first/middle/last edge, a prefix without a value, and a key extending a leaf.
  for (const auto *key : {"x", "ax", "abe", "a", "ab", "abcd", "abcde"}) {
    ASSERT_EQ(tree.Get<uint32_t>(key), nullptr);
    ASSERT_EQ(tree.Get<std::string>(key), nullptr);
  }
  const auto *root_value = tree.Get<uint32_t>(std::string_view{});
  const auto *leaf_value = tree.Get<uint32_t>("abc");
  const auto *text = tree.Get<std::string>("abde");
  const auto *owned = tree.Get<Integer>("z");
  ASSERT_NE(root_value, nullptr);
  ASSERT_NE(leaf_value, nullptr);
  ASSERT_NE(text, nullptr);
  ASSERT_NE(owned, nullptr);
  ASSERT_NE(owned->get(), nullptr);
  ASSERT_EQ(*root_value, 10U);
  ASSERT_EQ(*leaf_value, 20U);
  ASSERT_EQ(*text, "text");
  ASSERT_EQ(**owned, 30U);
  ASSERT_EQ(tree.Get<std::string>(""), nullptr);
  ASSERT_EQ(tree.Get<uint64_t>("abc"), nullptr);
  ASSERT_EQ(tree.Get<uint32_t>("abde"), nullptr);
  ASSERT_EQ(tree.Get<uint32_t>("z"), nullptr);
  const std::string backing = "__abcExtra";
  ASSERT_EQ(tree.Get<uint32_t>(std::string_view(backing.data() + 2, 3)), leaf_value);
  ASSERT_EQ(tree.Get<uint32_t>(std::string_view(backing.data() + 2, 4)), nullptr);
  ASSERT_EQ(tree.GetRoot(), root);  // Every lookup is read-only.
}

TEST(TrieTest, PutNodeTransitionMatrix) {
  struct Case {
    const char *name;
    std::vector<std::string> initial;
    std::string target;
    bool text;
  };
  const std::vector<Case> cases{
      {"empty tree, empty key", {}, "", false},
      {"empty tree, nonempty key", {}, "abc", false},
      {"missing first edge", {"a"}, "xyz", false},
      {"missing middle edge", {"abc"}, "axy", false},
      {"missing final edge", {"abc"}, "abd", false},
      {"non-value endpoint becomes value", {"abc", "abd"}, "ab", false},
      {"extend a valued leaf", {"a"}, "abc", false},
      {"extend a valued internal node", {"a", "abx"}, "aby", false},
      {"overwrite leaf, same type", {"abc"}, "abc", false},
      {"overwrite leaf, different type", {"abc"}, "abc", true},
      {"overwrite internal, same type", {"ab", "abc", "abd"}, "ab", false},
      {"overwrite internal, different type", {"ab", "abc", "abd"}, "ab", true},
      {"non-value root becomes value", {"a", "b"}, "", true},
      {"overwrite valued root with children", {"", "a", "b"}, "", true},
  };
  for (const auto &test : cases) {
    std::cout << "  put: " << test.name << '\n';
    Trie before;
    testutil::Model model;
    for (uint32_t i = 0; i < test.initial.size(); ++i) {
      before = before.Put<uint32_t>(test.initial[i], i + 1);
      model[test.initial[i]] = i + 1;
    }
    const auto old_model = model;
    auto after = test.text ? before.Put<std::string>(test.target, "replacement")
                          : before.Put<uint32_t>(test.target, 999);
    if (test.text) { model[test.target] = "replacement"; }
    else { model[test.target] = 999U; }
    auto queries = test.initial;
    queries.push_back(test.target);
    queries.push_back(test.target + "#missing");
    testutil::Check(before, old_model, queries);
    testutil::Check(after, model, queries);
    testutil::CheckShape(after, model);
    ASSERT_NE(after.GetRoot(), before.GetRoot());
    for (const auto &key : test.initial) {
      if (key != test.target) { ASSERT_EQ(before.Get<uint32_t>(key), after.Get<uint32_t>(key)); }
    }
  }
}

TEST(TrieTest, RemoveDecisionMatrix) {
  struct Case {
    const char *name;
    std::vector<std::string> initial;
    std::string target;
  };
  const std::vector<Case> cases{
      {"empty tree, empty key", {}, ""},
      {"empty tree, nonempty key", {}, "abc"},
      {"missing first edge", {"abc"}, "z"},
      {"missing middle edge", {"abc"}, "ax"},
      {"missing final edge", {"abc", "abd"}, "abe"},
      {"key extends a leaf", {"abc"}, "abcd"},
      {"endpoint exists without value", {"abc", "abd"}, "ab"},
      {"root exists without value", {"abc"}, ""},
      {"leaf with surviving sibling", {"a", "b"}, "a"},
      {"value with surviving children", {"a", "ab", "ac"}, "a"},
      {"root value with surviving children", {"", "a", "ab"}, ""},
      {"prune upward, stop at branching ancestor", {"abc", "az"}, "abc"},
      {"prune upward, stop at valued ancestor", {"a", "abc"}, "abc"},
      {"last nonempty key, prune through root", {"abc"}, "abc"},
      {"last empty key", {""}, ""},
      {"binary key with a valued ancestor", {"a", std::string("a\0b", 3)}, std::string("a\0b", 3)},
  };
  for (const auto &test : cases) {
    std::cout << "  remove: " << test.name << '\n';
    Trie before;
    testutil::Model model;
    for (uint32_t i = 0; i < test.initial.size(); ++i) {
      before = before.Put<uint32_t>(test.initial[i], i + 1);
      model[test.initial[i]] = i + 1;
    }
    const auto old_model = model;
    const bool existed = model.erase(test.target) != 0;
    const std::string backing = "__" + test.target + "#trailing-data";
    auto after = before.Remove(std::string_view(backing.data() + 2, test.target.size()));
    auto queries = test.initial;
    queries.push_back(test.target);
    testutil::Check(before, old_model, queries);
    testutil::Check(after, model, queries);
    testutil::CheckShape(after, model);
    if (!existed) { ASSERT_EQ(after.GetRoot(), before.GetRoot()); }
    else { ASSERT_NE(after.GetRoot(), before.GetRoot()); }
    for (const auto &entry : model) {
      ASSERT_EQ(before.Get<uint32_t>(entry.first), after.Get<uint32_t>(entry.first));
    }
    // Deleting again must be an identity operation, including when the tree is now empty.
    ASSERT_EQ(after.Remove(test.target).GetRoot(), after.GetRoot());
  }
}

TEST(TrieTest, AllBytesAndBoundedStringViews) {
  Trie tree;
  testutil::Model model;
  auto keys = testutil::Keys();
  for (uint32_t i = 0; i < keys.size(); ++i) {
    // The view has neither a leading boundary nor a trailing NUL in its backing storage.
    std::string backing = "before" + keys[i] + "after";
    std::string_view view(backing.data() + 6, keys[i].size());
    tree = tree.Put<uint32_t>(view, i);
    model[keys[i]] = i;
  }
  auto old = tree;
  auto old_model = model;
  for (std::size_t i = 0; i < keys.size(); i += 2) {
    tree = tree.Remove(keys[i]);
    model.erase(keys[i]);
  }
  testutil::Check(tree, model, keys);
  testutil::Check(old, old_model, keys);
}

TEST(TrieTest, PrefixLatticeAndTypeChanges) {
  Trie tree;
  testutil::Model model;
  std::vector<std::string> keys;
  for (uint32_t i = 0; i < 48; ++i) {
    auto prefix = std::string(i, 'p');
    keys.push_back(prefix);
    keys.push_back(prefix + "/leaf");
    tree = tree.Put<uint32_t>(prefix, i);
    tree = tree.Put<std::string>(prefix + "/leaf", "child");
    model[prefix] = i;
    model[prefix + "/leaf"] = "child";
  }
  const auto original = tree;
  const auto original_model = model;
  for (uint32_t i = 0; i < 48; ++i) {
    auto prefix = std::string(i, 'p');
    tree = tree.Put<std::string>(prefix, "changed");
    model[prefix] = "changed";
    testutil::Check(tree, model, keys);
    tree = tree.Remove(prefix);
    model.erase(prefix);
    testutil::Check(tree, model, keys);
  }
  testutil::Check(original, original_model, keys);
}

TEST(TrieTest, OnlyChangedPathsAreCopied) {
  auto base = Trie().Put<uint32_t>("abx", 1).Put<uint32_t>("aby", 2)
                   .Put<uint32_t>("az", 3).Put<uint32_t>("z", 4);
  auto updated = base.Put<std::string>("abx", "new type");
  auto old_node = base.GetRoot();
  auto new_node = updated.GetRoot();
  ASSERT_EQ(old_node->children_.at('z'), new_node->children_.at('z'));
  for (char ch : std::string("abx")) {
    ASSERT_NE(old_node, new_node);
    if (ch == 'b') {
      ASSERT_EQ(old_node->children_.at('z'), new_node->children_.at('z'));
    }
    if (ch == 'x') {
      ASSERT_EQ(old_node->children_.at('y'), new_node->children_.at('y'));
    }
    old_node = old_node->children_.at(ch);
    new_node = new_node->children_.at(ch);
  }
  ASSERT_NE(old_node, new_node);
  ASSERT_EQ(base.Get<uint32_t>("aby"), updated.Get<uint32_t>("aby"));
  auto removed = updated.Remove("abx");
  ASSERT_EQ(removed.GetRoot()->children_.at('z'), updated.GetRoot()->children_.at('z'));
  ASSERT_EQ(removed.Get<uint32_t>("aby"), updated.Get<uint32_t>("aby"));
  ASSERT_EQ(removed.Remove("missing").GetRoot(), removed.GetRoot());
  ASSERT_EQ(removed.Remove("ab").GetRoot(), removed.GetRoot());
  ASSERT_EQ(*base.Get<uint32_t>("abx"), 1U);
  ASSERT_EQ(*updated.Get<std::string>("abx"), "new type");
}

TEST(TrieTest, RandomBranchesAgainstMapOracle) {
  const auto keys = testutil::Keys();
  for (uint32_t seed : {17U, 445U, 2023U}) {
    std::mt19937 random(seed);
    struct Version { Trie tree; testutil::Model model; };
    std::vector<Version> versions(16);
    std::cout << "  branch seed=" << seed << '\n';
    for (uint32_t step = 0; step < 1800; ++step) {
      // Fork an arbitrary old version; replacing the current version alone is insufficient.
      auto next = versions[random() % versions.size()];
      const auto &key = keys[random() % keys.size()];
      switch (random() % 3) {
        case 0:
          next.tree = next.tree.Put<uint32_t>(key, step);
          next.model[key] = step;
          break;
        case 1: {
          auto value = std::to_string(seed) + ":" + std::to_string(step);
          next.tree = next.tree.Put<std::string>(key, value);
          next.model[key] = value;
          break;
        }
        default:
          next.tree = next.tree.Remove(key);
          next.model.erase(key);
      }
      testutil::Check(next.tree, next.model, keys);
      versions[random() % versions.size()] = std::move(next);
      if (step % 100 == 0) {
        for (const auto &version : versions) {
          testutil::Check(version.tree, version.model, keys);
        }
      }
    }
    for (const auto &version : versions) {
      testutil::Check(version.tree, version.model, keys);
    }
  }
}

TEST(TrieTest, LongKeysAndLargeSharedValues) {
  const std::string prefix(512, 'x');
  const std::string payload(1024 * 1024, 'v');
  auto original = Trie().Put<std::string>(prefix, payload)
                        .Put<uint32_t>(prefix + "a", 1).Put<uint32_t>(prefix + "b", 2);
  auto updated = original.Put<uint32_t>(prefix + "a", 3);
  ASSERT_EQ(original.Get<std::string>(prefix), updated.Get<std::string>(prefix));
  auto removed = updated.Remove(prefix);
  ASSERT_EQ(removed.Get<std::string>(prefix), nullptr);
  ASSERT_EQ(*removed.Get<uint32_t>(prefix + "a"), 3U);
  ASSERT_EQ(*removed.Get<uint32_t>(prefix + "b"), 2U);
  ASSERT_EQ(*original.Get<std::string>(prefix), payload);
  ASSERT_EQ(removed.Remove(prefix + "a").Remove(prefix + "b").GetRoot(), nullptr);
}

TEST(TrieTest, MoveOnlyBranchLifetimes) {
  std::array<std::atomic<int>, 17> destroyed{};
  for (auto &count : destroyed) { count.store(0); }
  {
    auto base = Trie().Put<TrackedPtr>("stable", std::make_unique<TrackedValue>(destroyed[16], 16));
    std::vector<Trie> branches;
    for (uint32_t i = 0; i < 16; ++i) {
      branches.push_back(base.Put<TrackedPtr>("changing", std::make_unique<TrackedValue>(destroyed[i], i)));
    }
    base = Trie();
    std::mt19937 random(445);
    std::shuffle(branches.begin(), branches.end(), random);
    for (auto &branch : branches) {
      const auto *value = branch.Get<TrackedPtr>("changing");
      ASSERT_NE(value, nullptr);
      ASSERT_NE(value->get(), nullptr);
      const auto id = (*value)->value_;
      auto removed = branch.Remove("changing");
      ASSERT_EQ(destroyed[id].load(), 0);
      branch = Trie();
      ASSERT_EQ(destroyed[id].load(), 1);
      ASSERT_EQ(destroyed[16].load(), 0);
      ASSERT_EQ((*removed.Get<TrackedPtr>("stable"))->value_, 16U);
    }
  }
  for (const auto &count : destroyed) { ASSERT_EQ(count.load(), 1); }
}

TEST(TrieTest, SnapshotCopiesShareRootAndKeepValues) {
  std::atomic<int> destroyed{0};
  auto tree = Trie().Put<TrackedPtr>("key", std::make_unique<TrackedValue>(destroyed, 42));
  const auto *value = tree.Get<TrackedPtr>("key");
  std::vector<Trie> copies(1024, tree);
  for (const auto &copy : copies) {
    ASSERT_EQ(copy.GetRoot(), tree.GetRoot());
    ASSERT_EQ(copy.Get<TrackedPtr>("key"), value);
  }
  tree = tree.Remove("key");
  ASSERT_EQ(destroyed.load(), 0);
  ASSERT_EQ((*copies.back().Get<TrackedPtr>("key"))->value_, 42U);
  copies.clear();
  ASSERT_EQ(destroyed.load(), 1);
}

TEST(TrieTest, FailedValueConstructionDoesNotChangeSnapshots) {
  auto original = Trie().Put<uint32_t>("key", 7);
  auto snapshot = original;
  std::promise<int> failure;
  failure.set_exception(std::make_exception_ptr(std::runtime_error("intentional move failure")));
  bool caught = false;
  try {
    original = original.Put<MoveBlocked>("key", MoveBlocked(failure.get_future()));
  } catch (const std::runtime_error &) {
    caught = true;
  }
  ASSERT_TRUE(caught);
  ASSERT_EQ(original.GetRoot(), snapshot.GetRoot());
  ASSERT_EQ(*snapshot.Get<uint32_t>("key"), 7U);
  auto updated = original.Put<uint32_t>("key", 8);
  ASSERT_EQ(*updated.Get<uint32_t>("key"), 8U);
}

TEST(TrieTest, RandomRemovalPrunesToEmpty) {
  auto keys = testutil::Keys();
  Trie tree;
  testutil::Model model;
  for (uint32_t i = 0; i < keys.size(); ++i) {
    tree = tree.Put<uint32_t>(keys[i], i);
    model[keys[i]] = i;
  }
  std::mt19937 random(314159);
  std::shuffle(keys.begin(), keys.end(), random);
  for (const auto &key : keys) {
    tree = tree.Remove(key);
    model.erase(key);
    ASSERT_EQ(tree.Remove(key).GetRoot(), tree.GetRoot());
    testutil::Check(tree, model, keys);
  }
  ASSERT_EQ(tree.GetRoot(), nullptr);
}
}  // namespace trie

int main(int argc, char **argv) { return labtest::Run(argc, argv); }
