#pragma once
// 页面扩展契约（架构第 8 节）：每个标签页实现 IPage 并把自身
// 注册进应用外壳的页面列表。第 3 阶段页面无需改动外壳即可接入。
#include <string>

namespace stm {

class AppContext;  // app/AppContext.h（打破循环：页面包含它，它包含本文件）

class IPage {
public:
    virtual ~IPage() = default;
    virtual const wchar_t* Id() const = 0;     // 供配置持久化的稳定 id
    virtual const wchar_t* Title() const = 0;  // 展示标题（中文）
    virtual void Draw(AppContext& ctx) = 0;    // 标签激活时每帧调用一次
};

}  // namespace stm
