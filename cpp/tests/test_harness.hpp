// Minimal header-only test harness.
//
// Deliberately not GoogleTest: this project has to build with nothing but a
// compiler and make, on a machine with no package manager guarantee. ~90 lines
// buys registration, assertions with file/line reporting, exception checking
// and a pass/fail summary, which is everything the tests below need.
//
//   TEST(order_book_matches_at_best_price) {
//     CHECK_EQ(book.best_bid(), 10000);
//   }
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, fn}); }
};

// Thrown by a failing assertion; caught by the runner so one bad test does not
// abort the rest of the suite.
struct Failure : std::exception {
  std::string message;
  explicit Failure(std::string m) : message(std::move(m)) {}
  const char* what() const noexcept override { return message.c_str(); }
};

template <typename T>
std::string repr(const T& v) {
  std::ostringstream os;
  os << v;
  return os.str();
}
inline std::string repr(bool v) { return v ? "true" : "false"; }

inline int run_all() {
  int failed = 0;
  int passed = 0;
  for (const auto& t : registry()) {
    try {
      t.fn();
      ++passed;
      std::printf("  [ ok ] %s\n", t.name);
    } catch (const Failure& f) {
      ++failed;
      std::printf("  [FAIL] %s\n         %s\n", t.name, f.what());
    } catch (const std::exception& e) {
      ++failed;
      std::printf("  [FAIL] %s\n         unexpected exception: %s\n", t.name, e.what());
    } catch (...) {
      ++failed;
      std::printf("  [FAIL] %s\n         unexpected non-standard exception\n", t.name);
    }
  }
  std::printf("\n%d passed, %d failed, %d total\n", passed, failed,
              static_cast<int>(registry().size()));
  return failed == 0 ? 0 : 1;
}

}  // namespace testing

#define TEST_CONCAT_(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_(a, b)

#define TEST(name)                                                          \
  static void name();                                                       \
  static ::testing::Registrar TEST_CONCAT(reg_, name)(#name, name);         \
  static void name()

#define FAIL_AT(msg)                                                        \
  throw ::testing::Failure(std::string(__FILE__) + ":" +                    \
                           std::to_string(__LINE__) + ": " + (msg))

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) FAIL_AT("CHECK failed: " #cond);                           \
  } while (0)

#define CHECK_FALSE(cond)                                                   \
  do {                                                                      \
    if ((cond)) FAIL_AT("CHECK_FALSE failed: " #cond);                      \
  } while (0)

#define CHECK_EQ(a, b)                                                      \
  do {                                                                      \
    const auto va_ = (a);                                                   \
    const auto vb_ = (b);                                                   \
    if (!(va_ == vb_)) {                                                    \
      FAIL_AT("CHECK_EQ failed: " #a " == " #b "\n           left  = " +    \
              ::testing::repr(va_) + "\n           right = " +              \
              ::testing::repr(vb_));                                        \
    }                                                                       \
  } while (0)

#define CHECK_NE(a, b)                                                      \
  do {                                                                      \
    if ((a) == (b)) FAIL_AT("CHECK_NE failed: " #a " != " #b);              \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                               \
  do {                                                                      \
    const double va_ = static_cast<double>(a);                              \
    const double vb_ = static_cast<double>(b);                              \
    const double d_ = va_ > vb_ ? va_ - vb_ : vb_ - va_;                    \
    if (!(d_ <= static_cast<double>(tol))) {                                \
      FAIL_AT("CHECK_NEAR failed: |" #a " - " #b "| <= " #tol               \
              "\n           left  = " +                                     \
              ::testing::repr(va_) + "\n           right = " +              \
              ::testing::repr(vb_));                                        \
    }                                                                       \
  } while (0)

#define CHECK_THROWS(expr)                                                  \
  do {                                                                      \
    bool threw_ = false;                                                    \
    try {                                                                   \
      (void)(expr);                                                         \
    } catch (...) {                                                         \
      threw_ = true;                                                        \
    }                                                                       \
    if (!threw_) FAIL_AT("CHECK_THROWS failed, no exception: " #expr);      \
  } while (0)
