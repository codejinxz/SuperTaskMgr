#pragma once
// F4#3 跨页联动：共享跳转状态与服务过滤（header-only 纯逻辑）。
//
// 单 UI 线程模型下用最简单的"槽位"语义：发起方写入，进程页在下一帧消费并清零。
// 消费失败（按 pid 找不到进程）由进程页负责 toast "进程已退出" —— 本头只搬运数据。
// 无 ImGui / 无 app 对象依赖；ops::ServiceInfo 仅作 POD 过滤输入。
#include <cstdint>
#include <vector>
#include "ops/ServiceOps.h"

namespace stm {
namespace ui3 {

struct JumpRequest {
    uint32_t pid = 0;
};

// 进程页跳转槽（单 UI 线程读写，无需加锁）。
inline JumpRequest& ProcessJumpSlot() {
    static JumpRequest s;
    return s;
}

// 任一页发起"跳转到进程"（pid==0 视为无目标，忽略）。
inline void RequestJumpToProcess(uint32_t pid) {
    if (pid == 0) return;
    ProcessJumpSlot().pid = pid;
}

// 进程页每帧消费；返回 true 时 *pid 为待定位目标并清空槽位。
inline bool ConsumeProcessJump(uint32_t* pid) {
    JumpRequest& s = ProcessJumpSlot();
    if (s.pid == 0) return false;
    if (pid != nullptr) *pid = s.pid;
    s.pid = 0;
    return true;
}

// 按 PID 过滤服务（EnumServices 结果 -> 该进程承载的服务；服务宿主共享 PID）。
inline std::vector<ops::ServiceInfo> FilterServicesByPid(const std::vector<ops::ServiceInfo>& svcs,
                                                         uint32_t pid) {
    std::vector<ops::ServiceInfo> out;
    for (const ops::ServiceInfo& s : svcs) {
        if (s.pid == pid) out.push_back(s);
    }
    return out;
}

}  // namespace ui3
}  // namespace stm
