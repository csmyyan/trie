#pragma once

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// 共用的最小测试工具，无外部依赖；Release 构建也执行断言。
namespace labtest {

struct TestCase {
  std::string name;
  void (*run)();
};

inline auto Cases() -> std::vector<TestCase> & {
  static std::vector<TestCase> cases;
  return cases;
}

struct Register {
  Register(const char *name, void (*run)()) { Cases().push_back({name, run}); }
};

inline void Check(bool passed, const char *expression, const char *file, int line) {
  if (!passed) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + expression);
  }
}

inline auto Pad(uint32_t value, std::size_t width) -> std::string {
  const auto text = std::to_string(value);
  return std::string(text.size() < width ? width - text.size() : 0, '0') + text;
}

// 可选参数按名称筛选测试，例如 TrieTest.BasicPutGetTest。
inline auto Run(int argc, char **argv) -> int {
  if (argc > 2) {
    std::cerr << "Usage: " << argv[0] << " [name-filter]\n";
    return 2;
  }
  int count = 0;
  int failed = 0;
  for (const auto &test : Cases()) {
    if (argc == 2 && test.name.find(argv[1]) == std::string::npos) {
      continue;
    }
    ++count;
    try {
      test.run();
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failed;
      std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
    } catch (...) {
      ++failed;
      std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
    }
  }
  std::cout << count << " tests, " << failed << " failed\n";
  return count == 0 ? 2 : (failed == 0 ? 0 : 1);
}

}  // namespace labtest

#define TEST(suite, name)                                      \
  static void suite##_##name();                               \
  static labtest::Register suite##_##name##_registration(      \
      #suite "." #name, suite##_##name);                       \
  static void suite##_##name()

#define ASSERT_TRUE(expression) labtest::Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define ASSERT_EQ(left, right) ASSERT_TRUE((left) == (right))
#define ASSERT_NE(left, right) ASSERT_TRUE((left) != (right))
