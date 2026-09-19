#pragma once
// 第 3 阶段页面可复用的异步数据抓取：页面侧缓存 + ops 任务提交，
// 带每页最小刷新间隔（网络 2s / 服务与驱动 5s /
// 传感器 10s），因此任何页面都不阻塞 UI 线程，
// 也不会每帧轰炸串行 ops 队列。
//
// 生命周期模型与 Pages.cpp 的 ops 任务一致：生产者 lambda 在
// JobQueue 工作线程上执行并捕获共享 AppContext，结果落入
// 共享的引用计数 State 单元。活得比应用久的任务（Shutdown
// 超时）只写它已持有的单元——绝不写页面对象。
//
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include "app/AppContext.h"
#include "app/ui3/Pages3.h"

namespace stm {
namespace ui3 {

template <typename T>
class AsyncFetch {
public:
    using ProduceFn = std::function<T(std::wstring*)>;
    struct Result {
        bool ok = false;        // err 返回为空时为 true
        T data{};               // 即使 err 已设置也允许部分数据
        std::wstring err;       // 面向用户的中文；成功时为空
    };

    explicit AsyncFetch(double minIntervalSec)
        : st_(std::make_shared<State>()), minIntervalSec_(minIntervalSec) {}

    // UI 线程。`force` 置位（手动刷新：绕过最小间隔——刷新按钮
    // 提示文案须与此一致）、首次抓取尚未发生、或距上次尝试
    // 已过最小间隔时提交生产任务。任务入队成功返回 true。
    // 任务在途期间每次调用都是空操作。
    //
    bool MaybeFetch(const ProduceFn& fn, bool force) {
        std::shared_ptr<AppContext> app = LiveP3Ctx();
        if (!app) return false;
        std::shared_ptr<State> st = st_;
        bool submit = false;
        {
            std::lock_guard<std::mutex> lock(st->mu);
            const double now = SteadySec();
            if (!st->busy &&
                (force || !st->everFetched || now - st->lastAttempt >= minIntervalSec_)) {
                st->busy = true;
                st->everFetched = true;
                st->lastAttempt = now;
                submit = true;
            }
        }
        if (!submit) return false;
        // 队列未运行（拆除中）时 Submit 返回 0：撤销。
        if (app->jobs.Submit([app, st, fn] {
                std::wstring err;
                T data = fn(&err);
                std::lock_guard<std::mutex> lock(st->mu);
                st->result = std::make_shared<const Result>(
                    Result{err.empty(), std::move(data), std::move(err)});
                st->busy = false;
            }) == 0) {
            std::lock_guard<std::mutex> lock(st->mu);
            st->busy = false;
            return false;
        }
        return true;
    }

    // 最近完成的结果；首个任务完成前为 nullptr。
    std::shared_ptr<const Result> Peek() const {
        std::lock_guard<std::mutex> lock(st_->mu);
        return st_->result;
    }

    bool Busy() const {
        std::lock_guard<std::mutex> lock(st_->mu);
        return st_->busy;
    }

private:
    struct State {
        std::mutex mu;
        std::shared_ptr<const Result> result;
        bool busy = false;
        bool everFetched = false;
        double lastAttempt = 0.0;
    };

    static double SteadySec() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    std::shared_ptr<State> st_;
    double minIntervalSec_;
};

}  // namespace ui3
}  // namespace stm
