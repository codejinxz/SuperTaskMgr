// CollectService 实现（架构第 4-5 节）：单一采集线程，
// 每个间隔产出一个 Snapshot 写入 SnapshotStore。
//   - 间隔限制在 500-5000 ms，默认 1000；SetInterval 会唤醒工作线程，
//     新间隔立即生效。
//   - tick 延迟统计：最近值 + 120 样本滚动窗口的 p95。
//   - GPU/PDH 查询按自身 2s 节拍运行；若其 p95 超过 10 ms，
//     节拍自动降级为 4s（单向，仅告警一次）。
//   - Stop 干净地 join；异常绝不逃出工作线程。
#include "collect/CollectService.h"
#include "collect/CollectDetail.h"
#include "collect/SelfCheckGate.h"  // 维护轮 10：自检门逐项报告（GateReport）
#include "core/Log.h"
#include "core/Str.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

namespace stm {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMinIntervalMs = 500;
constexpr uint32_t kMaxIntervalMs = 5000;

uint32_t ClampInterval(uint32_t ms) {
    return ms < kMinIntervalMs ? kMinIntervalMs : (ms > kMaxIntervalMs ? kMaxIntervalMs : ms);
}

// 滚动延迟窗口；对最近样本取 P95(lastN)。
template <size_t N>
class LatencyRing {
public:
    void Push(double ms) {
        buf_[count_ < N ? count_ : idx_] = ms;
        idx_ = (idx_ + 1) % N;
        if (count_ < N) ++count_;
    }
    size_t Count() const { return count_; }
    double P95(size_t lastN) const {
        const size_t n = std::min(count_, lastN == 0 ? count_ : lastN);
        if (n == 0) return 0.0;
        std::vector<double> v;
        v.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            v.push_back(buf_[(idx_ + N - 1 - i) % N]);  // 最新的在前
        }
        const size_t k = static_cast<size_t>(0.95 * static_cast<double>(n - 1));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        return v[k];
    }

private:
    std::array<double, N> buf_{};
    size_t idx_ = 0, count_ = 0;
};

}  // namespace

struct CollectService::Impl {
    // ---- 线程控制 ----
    std::thread worker;
    std::mutex mu;
    std::condition_variable cv;
    bool running = false;
    uint32_t intervalMs = 1000;
    uint64_t intervalGen = 0;  // 由 SetInterval 递增以唤醒工作线程
    // ---- 开关 ----
    bool gpuEnabled = true;
    bool netEtwEnabled = false;
    // ---- 统计（由 mu 保护）----
    uint64_t ticks = 0;
    double lastTickMs = 0.0;
    LatencyRing<120> tickRing;
    LatencyRing<120> gpuRing;
    // ---- 采集器 ----
    cd::ProcessCollector procCol;
    cd::SystemCollector sysCol;
    cd::GpuCollector gpuCol;
    // ---- ETW 每 pid 网络速率（第 3 阶段；默认关，仅管理员）------
    cd::EtwNetCollector netEtw;
    std::unordered_map<uint32_t, uint64_t> prevNetBytes_;  // pid -> 上一 tick 的累计字节
    // ---- 自检门限（只在第一个 tick 运行一次；RequestSelfCheckRetry 触发重跑）----
    bool gateDone = false;
    bool degraded = false;
    std::wstring degradeReason;
    // ---- 兼容模式诊断（维护轮 10；V24 P1-1 加代际）----
    // retry：UI/selftest 置位（原子），采集线程在下一 tick 用 exchange 消费。
    std::atomic<bool> selfCheckRetry{false};
    // 门每真正跑完一次（报告已写入 lastSelfCheck 之后）+1；UI 据此判定
    // 一次重跑完成——ticks++ 发生在 DoTick 之后、标志在 DoTick 开头消费，
    // tick 计数会"提前"于重跑完成，不能作为完成信号。
    std::atomic<uint64_t> gateRunGen{0};
    // 最近一次自检门的逐项结果（mu 保护；LastSelfCheckReport 任意线程可调）。
    std::vector<SelfCheckItem> lastSelfCheck;
    // ---- GPU 节拍 + 最近 GPU 数据（跳过查询的 tick 复用）----
    Clock::time_point nextGpuAt{};  // epoch -> 首个 tick 即到期
    double gpuIntervalMs = 2000.0;
    bool gpuDegradeLogged = false;
    std::vector<GpuProcUsage> lastGpuProcs;
    std::vector<GpuAdapterInfo> lastGpus;

    void Run(SnapshotStore* store) {
        uint64_t tickId = 0;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mu);
                if (!running) return;
            }
            const auto t0 = Clock::now();
            ++tickId;
            try {
                DoTick(store, tickId);
            } catch (const std::exception& e) {
                STM_LOG_ERROR("collect", Fmt(L"采集 tick 异常：{}", Utf8ToWide(e.what())));
            } catch (...) {
                STM_LOG_ERROR("collect", L"采集 tick 未知异常");
            }
            const auto t1 = Clock::now();
            {
                std::lock_guard<std::mutex> lock(mu);
                ++ticks;
                lastTickMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                tickRing.Push(lastTickMs);
            }
            // 睡到下一个 tick；Stop() 运行或间隔变化时提前唤醒
            //（SetInterval 必须立即生效）。
            std::unique_lock<std::mutex> lock(mu);
            for (;;) {
                if (!running) return;
                const uint32_t iv = intervalMs;
                const uint64_t gen = intervalGen;
                const auto target = Clock::now() + std::chrono::milliseconds(iv);
                cv.wait_until(lock, target,
                              [&] { return !running || intervalGen != gen; });
                if (!running) return;
                if (intervalGen != gen) continue;  // 间隔已变：重算目标时刻
                break;                             // 自然超时 -> 进入下一 tick
            }
        }
    }

    void DoTick(SnapshotStore* store, uint64_t tickId) {
        // 自检门限：启动第一个 tick 运行；之后仅当收到 RequestSelfCheckRetry
        //（原子标志，exchange 消费——多次请求合并为一次重跑）时重跑。
        // 通过则自动退出兼容模式（degraded 归零，后续快照回到快速路径）。
        if (!gateDone || selfCheckRetry.exchange(false)) {
            gateDone = true;
            const cd::GateReport rep = cd::RunSelfCheckGateReported();
            degraded = rep.result.degraded;
            degradeReason = rep.result.reason;
            {   // 逐项报告：name 指向静态字面量，指针复制安全；detail 深拷贝。
                std::vector<SelfCheckItem> items;
                items.reserve(rep.items.size());
                for (const cd::GateItem& gi : rep.items) {
                    items.push_back(SelfCheckItem{gi.name, gi.ran, gi.passed, gi.detail});
                }
                std::lock_guard<std::mutex> lock(mu);
                lastSelfCheck = std::move(items);
            }
            // P1-1：报告可见之后才递增代际（release/acquire 配对），UI 观察到
            // 新代际时 LastSelfCheckReport() 必然已是本次结果。
            gateRunGen.fetch_add(1, std::memory_order_release);
            if (degraded) {
                STM_LOG_WARN("collect", Fmt(L"自校验门：{}", degradeReason));
            } else {
                STM_LOG_INFO("collect", L"自校验门通过，NtQSI 快速路径启用（重跑或首次）");
            }
        }

        auto snap = std::make_shared<Snapshot>();
        snap->tickId = tickId;
        snap->degraded = degraded;
        snap->degradeReason = degradeReason;
        snap->caps = 0;  // 仅在 ETW 实际运行时才在下方置位 CAP_NET_ETW

        cd::ProcessCollector::TickOut pt;
        procCol.Collect(tickId, degraded, &pt);
        snap->procs = std::move(pt.procs);  // 已按 pid 升序
        snap->tickSec = pt.elapsedSec;      // 本 tick 使用的差值归一化
        if (!pt.ntsiOk && !degraded) {
            // 通过门限后运行期 NtQSI 失败：诚实降级。
            snap->degraded = true;
            snap->degradeReason = L"NtQuerySystemInformation 本次调用失败，本 tick 使用兼容路径";
        }

        // --- ETW 每进程网络速率（第 3 阶段，R5 #10b）-------------------
        // EtwNetCollector 在其消费线程上累计每 pid 的收发总字节（累计值）；
        //（pid -> 字节/秒）差分在此处的合成阶段完成，而非放在
        // ProcessCollector 内部：ETW 状态保存在本 Impl 中，这样改动最小，
        // 且 ProcessCollector 快路径保持不动。
        // ETW 事件只携带 pid（无 createTime）：窗口中途 pid 被复用
        // 属于第 3 阶段的近似处理。
        {
            std::lock_guard<std::mutex> lock(mu);
            const bool etwOn = netEtwEnabled;
            if (etwOn && netEtw.Running()) {
                std::unordered_map<uint32_t, uint64_t> cum;
                netEtw.CopyCumulative(&cum);
                if (!prevNetBytes_.empty() && pt.elapsedSec > 0.0) {
                    for (ProcInfo& p : snap->procs) {
                        const auto it = cum.find(p.key.pid);
                        const auto pit = prevNetBytes_.find(p.key.pid);
                        if (it == cum.end() || pit == prevNetBytes_.end()) continue;
                        const uint64_t now = it->second;
                        const uint64_t was = pit->second;
                        if (now >= was) {  // 计数器回退（重启）-> 跳过本 tick
                            p.netBytesPerSec = static_cast<double>(now - was) / pt.elapsedSec;
                        }
                    }
                }
                prevNetBytes_ = std::move(cum);
                snap->caps |= CAP_NET_ETW;
            } else {
                prevNetBytes_.clear();
            }
        }

        sysCol.Collect(pt, &snap->sys);

        // GPU 按自身节拍运行；中间的 tick 复用最近一次结果。
        // gpuIntervalMs 仅工作线程访问，这里无需加锁。
        if (gpuEnabled) {
            const auto now = Clock::now();
            if (now >= nextGpuAt) {
                nextGpuAt =
                    now + std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double, std::milli>(gpuIntervalMs));
                std::vector<GpuProcUsage> gp;
                std::vector<GpuAdapterInfo> ga;
                double qms = 0.0;
                gpuCol.Collect(pt.createTimeByPid, &gp, &ga, &qms);
                {
                    std::lock_guard<std::mutex> lock(mu);
                    gpuRing.Push(qms);
                    if (gpuRing.Count() >= 5) {
                        const double p = gpuRing.P95(20);
                        if (p > 10.0 && gpuIntervalMs < 4000.0) {
                            gpuIntervalMs = 4000.0;  // 自动降级，单向
                            if (!gpuDegradeLogged) {
                                gpuDegradeLogged = true;
                                STM_LOG_WARN("collect",
                                             Fmt(L"GPU 查询 p95 {:.1f}ms > 10ms，节奏降为 4s", p));
                            }
                        }
                    }
                }
                lastGpuProcs = std::move(gp);
                lastGpus = std::move(ga);
            }
            snap->gpuProcs = lastGpuProcs;
            snap->sys.gpus = lastGpus;
        }

        snap->timestamp = static_cast<int64_t>(time(nullptr));
        store->Set(std::move(snap));
    }
};

// ---------------------------------------------------------------------------
// CollectService（pimpl 转发）
// ---------------------------------------------------------------------------
CollectService::CollectService() : impl_(std::make_unique<Impl>()) {}

CollectService::~CollectService() { Stop(); }

bool CollectService::Start(uint32_t intervalMs) {
    SetInterval(intervalMs);
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->running) return true;
    impl_->running = true;
    try {
        impl_->worker = std::thread(&Impl::Run, impl_.get(), &store_);
    } catch (...) {
        impl_->running = false;
        return false;
    }
    return true;
}

void CollectService::Stop() {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (!impl_->running && !impl_->worker.joinable()) return;
        impl_->running = false;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void CollectService::SetInterval(uint32_t ms) {
    const uint32_t clamped = ClampInterval(ms);
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (impl_->intervalMs == clamped) return;
        impl_->intervalMs = clamped;
        ++impl_->intervalGen;
    }
    impl_->cv.notify_all();
}

uint64_t CollectService::TickCount() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->ticks;
}

double CollectService::LastTickMs() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->lastTickMs;
}

double CollectService::TickP95Ms() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->tickRing.P95(0);
}

double CollectService::GpuTickP95Ms() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->gpuRing.P95(0);
}

void CollectService::SetGpuEnabled(bool on) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->gpuEnabled = on;
}

bool CollectService::GpuEnabled() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->gpuEnabled;
}

// ---- 兼容模式诊断（维护轮 10，契约见 CollectService.h）--------------------

std::vector<CollectService::SelfCheckItem> CollectService::LastSelfCheckReport() const {
    // 空向量 = 尚未自检（采集线程第一个 tick 之前）。name 指向
    // 采集内部的静态字面量，detail 为深拷贝——返回值可安全跨线程持有。
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->lastSelfCheck;
}

void CollectService::RequestSelfCheckRetry() {
    // 只置原子标志，绝不在这里跑自检（本调用来自 UI/selftest 线程）；
    // 采集线程在下一 tick 用 exchange 消费。成功则自动退出兼容模式。
    impl_->selfCheckRetry.store(true, std::memory_order_release);
    STM_LOG_INFO("selfcheck", L"收到重新自检请求：下一采集 tick 重跑自检门（多次请求合并）");
}

uint64_t CollectService::LastSelfCheckGeneration() const {
    // V24 P1-1：完成检测的单一事实来源——门真跑完（含报告落库）才 +1。
    return impl_->gateRunGen.load(std::memory_order_acquire);
}

void CollectService::SetNetEtwEnabled(bool on) {
    // Start/Stop 会短暂阻塞（会话控制 + 消费线程 join）但绝不回调
    // 进入服务本身，因此持 mu_ 是安全的；加锁顺序始终是
    // Impl::mu_ -> EtwNetCollector::mu_。
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->netEtwEnabled == on) return;
    if (on) {
        // 失败（非管理员等）在 Start 内部记日志，功能保持禁用——
        // 诚实，绝不半启用。
        if (impl_->netEtw.Start()) impl_->netEtwEnabled = true;
    } else {
        impl_->netEtw.Stop();
        impl_->netEtwEnabled = false;
    }
}

bool CollectService::NetEtwEnabled() const {
    // V9 P0-4：读回的必须是真实状态，而不只是请求标志——
    // 启动失败（标志从未置位）与启动成功后会话死掉（标志过期）
    // 两种情况都返回 false，
    // 让 UI 能诚实地把开关回拨。
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->netEtwEnabled && impl_->netEtw.Running();
}

}  // namespace stm
