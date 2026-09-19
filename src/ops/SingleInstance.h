#pragma once
// 单实例命名互斥体（架构第 7 节）。Local\ 命名空间：按登录会话隔离。
#include "core/HandleGuard.h"

namespace stm {
namespace ops {

class SingleInstance {
public:
    // 尝试获取 "Local\SuperTaskMgr.SingleInstance"；至多等待 waitMs
    //（提权重启握手依赖旧实例释放它）。
    bool TryAcquire(uint32_t waitMs);
    bool Acquired() const { return handle_ != nullptr; }

private:
    UniqueHandle handle_;
};

}  // namespace ops
}  // namespace stm
