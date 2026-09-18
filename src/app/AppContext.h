#pragma once
// Application context handed to every IPage (arch section 8). Contract header — architect-owned.
// Lifetime: constructed in main before the message loop; services started before pages draw.
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
    JobQueue jobs;                    // ops worker queue
    NotificationQueue notes;          // worker -> UI results
    std::unique_ptr<ops::DetailsProvider> details;

    bool elevated = false;
    // Atomic: worker jobs (elevate relaunch, tray) set this; the UI loop polls it.
    std::atomic<bool> wantExit{false};
    // P3 任务三：标题栏 X 的关闭询问。main 的 onMessage 拦截 WM_CLOSE 后按
    // cfg closeAction 分派；0（每次询问）置位本标志，DrawConfirmDialogs 每帧
    // 消费并弹出三选一模态（同一 F1 修复模式：请求长期有效，绝不单帧渲染）。
    std::atomic<bool> closeAskPending{false};
    // P3 任务三：主窗 HWND（窗口创建后由 main 写入），供「最小化到托盘」的
    // ShowWindow(SW_HIDE) 使用。保持 void* 以免本契约头引入 OS 头文件。
    void* mainHwnd = nullptr;
    // Status bar self-observability (UI thread updates each frame).
    double frameMs = 0;

    // Page registry (app shell owns instances; order = tab order).
    std::vector<std::unique_ptr<IPage>> pages;
    int activePage = 0;
};

}  // namespace stm
