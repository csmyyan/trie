#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <utility>

namespace trie {

class MoveBlocked {
 public:
  explicit MoveBlocked(std::future<int> wait, std::promise<void> *entered = nullptr, uint32_t value = 0)
      : wait_(std::move(wait)), entered_(entered), value_(std::make_unique<uint32_t>(value)) {}
  MoveBlocked(const MoveBlocked &) = delete;
  auto operator=(const MoveBlocked &) -> MoveBlocked & = delete;
  MoveBlocked(MoveBlocked &&other) {
    other.Wait();
    value_ = std::move(other.value_);
  }
  auto operator=(MoveBlocked &&other) -> MoveBlocked & {
    other.Wait();
    value_ = std::move(other.value_);
    wait_ = {};
    entered_ = nullptr;
    return *this;
  }

  auto GetValue() const -> const uint32_t * { return value_.get(); }

 private:
  void Wait() {
    if (wait_.valid()) {
      if (entered_ != nullptr) {
        entered_->set_value();
      }
      wait_.get();
    }
  }

  std::future<int> wait_;
  std::promise<void> *entered_{nullptr};
  std::unique_ptr<uint32_t> value_;
};

}  // namespace trie
