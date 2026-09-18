#pragma once
// F4#4 进程树视图：快照 -> 显示行序（header-only 纯逻辑，UI 与 stm_selftest 共用）。
//
// 规则（架构 §6 (pid, createTime) 防复用同款纪律）：
//  - 父子按 parentPid 链接；父进程必须存在于同一快照，且父 createTime 不晚于子
//    createTime（防止 PID 复用把进程挂到无关父进程）——异常链接一律"提升为根"。
//  - 输出为 DFS 行序；兄弟与根均按调用方给定的排序键（当前表格排序列）排序。
//  - 过滤与树形叠加时由调用方"先过滤后建树"：直接传入过滤后的 procs 子集即可。
//  - 父子互指成环的节点永远不会自然成为根：第二轮把未访问节点按序提升为根，
//    绝不丢行（诚实原则）。
// 无 ImGui / 无 OS 依赖。
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>
#include "core/ProcData.h"

namespace stm {
namespace ui3 {

// 树形模式名称列的缩进与连接符（渲染细节常数，测试锁定数值）。
constexpr float kTreeIndentPx = 12.0f;  // 每层缩进像素
inline const wchar_t* TreeBranchGlyph() { return L"└ "; }

// 一行显示数据：index = 快照 procs 下标；depth = 0 为根。
struct TreeRow {
    int index = -1;
    int depth = 0;
};

// less(a, b)：procs 下标 a、b 的严格序（根与兄弟都按它排序；UI 传当前 SortLess）。
template <typename Less>
inline void BuildTreeOrder(const std::vector<ProcInfo>& procs, Less less,
                           std::vector<TreeRow>* out) {
    out->clear();
    const int n = static_cast<int>(procs.size());
    if (n <= 0) return;

    // pid -> 首个下标（快照按 pid 升序，重复 pid 理论上不存在，取首个为诚实降级）。
    std::unordered_map<uint32_t, int> byPid;
    byPid.reserve(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) byPid.emplace(procs[static_cast<size_t>(i)].key.pid, i);

    std::vector<int> parent(static_cast<size_t>(n), -1);
    std::vector<std::vector<int>> children(static_cast<size_t>(n));
    std::vector<int> roots;
    for (int i = 0; i < n; ++i) {
        const ProcInfo& p = procs[static_cast<size_t>(i)];
        const uint32_t pp = p.parentPid;
        if (pp == 0 || pp == p.key.pid) {
            roots.push_back(i);  // 无父（系统根）或自引用异常
            continue;
        }
        const auto it = byPid.find(pp);
        if (it == byPid.end()) {
            roots.push_back(i);  // 孤儿：父不在快照
            continue;
        }
        const ProcInfo& par = procs[static_cast<size_t>(it->second)];
        if (par.key.createTime > p.key.createTime) {
            roots.push_back(i);  // 父 createTime 晚于子：判定为 PID 复用，提升为根
            continue;
        }
        parent[static_cast<size_t>(i)] = it->second;
        children[static_cast<size_t>(it->second)].push_back(i);
    }

    for (std::vector<int>& c : children) std::stable_sort(c.begin(), c.end(), less);
    std::stable_sort(roots.begin(), roots.end(), less);

    std::vector<char> visited(static_cast<size_t>(n), 0);
    std::vector<std::pair<int, int>> stack;  // (下标, 深度)，迭代 DFS 防深链递归
    const auto dfs = [&](int rootIdx, int depth) {
        stack.push_back({rootIdx, depth});
        while (!stack.empty()) {
            const std::pair<int, int> top = stack.back();
            stack.pop_back();
            if (visited[static_cast<size_t>(top.first)]) continue;
            visited[static_cast<size_t>(top.first)] = 1;
            out->push_back(TreeRow{top.first, top.second});
            const std::vector<int>& ch = children[static_cast<size_t>(top.first)];
            for (auto it = ch.rbegin(); it != ch.rend(); ++it) {
                if (!visited[static_cast<size_t>(*it)]) stack.push_back({*it, top.second + 1});
            }
        }
    };
    for (int r : roots) dfs(r, 0);
    // 环上节点（互相为父）两轮兜底：按序提升为根，保证每行恰好输出一次。
    for (int i = 0; i < n; ++i) {
        if (!visited[static_cast<size_t>(i)]) dfs(i, 0);
    }
}

}  // namespace ui3
}  // namespace stm
