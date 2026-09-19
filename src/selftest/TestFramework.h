#pragma once
// 微型自测注册表。每个模块在自己的编译单元里用 STM_TEST 宏添加用例；
// main.cpp 运行全部并打印 PASS/FAIL（或 --json）。
// 失败细节写入 *err（中文，面向用户）。
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

// 示例：
//   STM_TEST(core_fmt_roundtrip) { ... if (bad) { *err = L"..."; return false; } return true; }
#define STM_TEST(testName)                                                        \
    static bool testName##_impl(std::wstring* err);                               \
    static const int testName##_reg =                                             \
        ::stmtest::Register(#testName, &testName##_impl);                         \
    static bool testName##_impl(std::wstring* err)
