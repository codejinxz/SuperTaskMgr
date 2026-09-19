#pragma once
// 交给每个 IPage 的应用上下文（架构第 8 节）。契约头——归架构所有。
// 生命周期：在 main 中于消息循环前构造；服务在页面绘制前启动。
#include <atomic>
#include <memory>
#include <vector>
#include "core/Cfg.h"
#include "core/Jobs.h"
#include "core/Notifications.h"
#include "collect/CollectService.h"
#include "ops/DetailsProvider.h"
#include "ui/IPage.h"

namespace stm {

class AppContext {
public:
    Config cfg;
    CollectService collect;
    JobQueue jobs;                    // ops 工作队列
    NotificationQueue notes;          // 工作线程 -> UI 的结果
    std::unique_ptr<ops::DetailsProvider> details;

    bool elevated = false;
    // 原子量：工作任务（提权重启、托盘）设置它；UI 循环轮询。
    std::atomic<bool> wantExit{false};
    // P3 任务三：标题栏 X 的关闭询问。main 的 onMessage 拦截 WM_CLOSE 后按
    // cfg closeAction 分派；0（每次询问）置位本标志，DrawConfirmDialogs 每帧
    // 消费并弹出三选一模态（同一 F1 修复模式：请求长期有效，绝不单帧渲染）。
    std::atomic<bool> closeAskPending{false};
    // P3 任务三：主窗 HWND（窗口创建后由 main 写入），供「最小化到托盘」的
    // ShowWindow(SW_HIDE) 使用。保持 void* 以免本契约头引入 OS 头文件。
    void* mainHwnd = nullptr;
    // 状态栏自观测（UI 线程每帧更新）。
    double frameMs = 0;

    // 页面注册表（应用外壳持有实例；顺序即标签页顺序）。
    std::vector<std::unique_ptr<IPage>> pages;
    int activePage = 0;
};

}  // namespace stm
