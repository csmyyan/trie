#pragma once

#include <stdexcept>

namespace trie {

// 仅用于标记尚未实现的函数。
class NotImplementedException : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};

}  // namespace trie
