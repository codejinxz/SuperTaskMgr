#pragma once
// SelfCheckGate 逐项诊断（维护轮 10）。本头归采集实现所有（SelfCheckGate.*），
// 是 CollectDetail.h 之外的一个小补充：CollectDetail.h 声明的
// RunSelfCheckGate()（无逐项结果）原样保留并在 SelfCheckGate.cpp 中
// 委托到本头的加固版；CollectService.h 的契约不受影响。
#include "collect/CollectDetail.h"  // GateResult / NtProcRow
#include <string>
#include <vector>

namespace stm {
namespace cd {

// 与契约层 CollectService::SelfCheckItem 同构的采集内部表示
//（CollectService 负责把它转成契约类型）。name 指向静态字符串字面量，
// 进程生命周期内恒有效，可安全跨线程复制。
struct GateItem {
    const wchar_t* name = L"";  // 检查项名（如 L"CPU 时间字段"）
    bool ran = false;           // 本轮是否执行了该项（false = 有据可查的跳过）
    bool passed = false;
    std::wstring detail;        // 测量值对比 / 跳过原因（恒非空，诊断报告用）
};

struct GateReport {
    GateResult result;             // degraded + 一行原因（状态栏"兼容模式：<原因>"）
    std::vector<GateItem> items;   // 恒 6 项、顺序固定（见 SelfCheckGate.cpp 的项名）
};

// 加固版启动自检门（原 RunSelfCheckGate 的逐项报告 + 重试加固）：
//  - 每轮对至多 3 个参考进程把 NtQSI 半文档化结构读数与文档化 API
//    交叉比对 6 项（CPU 时间/工作集/IO/句柄/线程/私有工作集）。
//  - 参考进程每轮重选：优先自身进程，其次存活 > 60s 的系统进程
//   （映像路径在系统目录下，或已知系统映像名），按创建时间从旧到新取前 3
//   ——消除"参考进程中途退出/刚启动进程自身计数还在抖动"两类误报源。
//  - 任一轮未通过自动重试，至多再试 2 次（间隔 200ms）；连续 2 轮全部
//    通过才判通过，仍失败才降级到 Toolhelp+PSAPI 兼容路径——吸收单次
//    采样噪声与安全软件对 NtQuerySystemInformation 的间歇性篡改。
//  - 全部轮次、逐项判定与重试决策写日志（模块 "selfcheck"）。绝不抛异常。
GateReport RunSelfCheckGateReported();

}  // namespace cd
}  // namespace stm
