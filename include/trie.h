#pragma once

#include <map>
#include <memory>
#include <string_view>
#include <utility>

namespace trie {

class TrieNode {
 public:
  using Children = std::map<char, std::shared_ptr<const TrieNode>>;

  TrieNode() = default;
  explicit TrieNode(const Children &children) : children_(children) {}
  virtual ~TrieNode() = default;

  virtual auto Clone() const -> std::shared_ptr<TrieNode> { return std::make_shared<TrieNode>(children_); }

  Children children_;
  bool is_value_node_{false};
};

template <class T>
class TrieNodeWithValue : public TrieNode {
 public:
  explicit TrieNodeWithValue(std::shared_ptr<T> value) : value_(std::move(value)) {
    is_value_node_ = true;
  }

  TrieNodeWithValue(const Children &children, std::shared_ptr<T> value)
      : TrieNode(children), value_(std::move(value)) {
    is_value_node_ = true;
  }

  auto Clone() const -> std::shared_ptr<TrieNode> override {
    return std::make_shared<TrieNodeWithValue<T>>(children_, value_);
  }

  std::shared_ptr<T> value_;
};

class Trie {
 public:
  Trie() = default;

  template <class T>
  auto Get(std::string_view key) const -> const T *;

  template <class T>
  auto Put(std::string_view key, T value) const -> Trie {
    return Put<T>(key, std::make_shared<T>(std::move(value)));
  }

  auto Remove(std::string_view key) const -> Trie;

  auto GetRoot() const noexcept -> const std::shared_ptr<const TrieNode> & { return root_; }

 private:
  friend class CasTrieStore;

  template <class T>
  auto Put(std::string_view key, std::shared_ptr<T> value) const -> Trie;

  explicit Trie(std::shared_ptr<const TrieNode> root) : root_(std::move(root)) {}
  std::shared_ptr<const TrieNode> root_;
};

}  // namespace trie
