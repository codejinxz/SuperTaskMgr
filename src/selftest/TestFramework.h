#pragma once
// Tiny self-test registry. Each module adds cases with the STM_TEST macro in its own
// translation unit; main.cpp runs everything and prints PASS/FAIL (or --json).
// Any failure detail is written into *err (Chinese, user-facing).
#include <string>
#include <vector>

namespace stmtest {

struct TestCase {
    const char* name;
    bool (*fn)(std::wstring* err);
};

std::vector<TestCase>& Registry();
inline int Register(const char* name, bool (*fn)(std::wstring* err)) {
    Registry().push_back({name, fn});
    return 0;
}

}  // namespace stmtest

// Example:
//   STM_TEST(core_fmt_roundtrip) { ... if (bad) { *err = L"..."; return false; } return true; }
#define STM_TEST(testName)                                                        \
    static bool testName##_impl(std::wstring* err);                               \
    static const int testName##_reg =                                             \
        ::stmtest::Register(#testName, &testName##_impl);                         \
    static bool testName##_impl(std::wstring* err)
