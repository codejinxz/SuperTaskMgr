// CollectService implementation (arch sections 4-5): a single collection thread
// producing one Snapshot per interval into the SnapshotStore.
//   - interval 500-5000 ms clamped, default 1000; SetInterval wakes the worker
//     so a new interval takes effect immediately.
//   - tick latency stats: last + p95 over a rolling window of 120 samples.
//   - GPU/PDH queries run on their own 2 s cadence; if their p95 exceeds 10 ms
//     the cadence automatically degrades to 4 s (one-way, warned once).
//   - Stop joins cleanly; no exceptions ever escape the worker.
#include "collect/CollectService.h"
#include "collect/CollectDetail.h"
#include "core/Log.h"
#include "core/Str.h"
#include <algorithm>
#include <array>
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

// Rolling latency window; P95(lastN) over the most recent samples.
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
            v.push_back(buf_[(idx_ + N - 1 - i) % N]);  // newest first
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
    // ---- thread control ----
    std::thread worker;
    std::mutex mu;
    std::condition_variable cv;
    bool running = false;
    uint32_t intervalMs = 1000;
    uint64_t intervalGen = 0;  // bumped by SetInterval to wake the worker
    // ---- toggles ----
    bool gpuEnabled = true;
    bool netEtwEnabled = false;
    // ---- stats (guarded by mu) ----
    uint64_t ticks = 0;
    double lastTickMs = 0.0;
    LatencyRing<120> tickRing;
    LatencyRing<120> gpuRing;
    // ---- collectors ----
    cd::ProcessCollector procCol;
    cd::SystemCollector sysCol;
    cd::GpuCollector gpuCol;
    // ---- ETW per-pid network rates (phase 3; default OFF, admin only) ------
    cd::EtwNetCollector netEtw;
    std::unordered_map<uint32_t, uint64_t> prevNetBytes_;  // pid -> cumulative bytes at prev tick
    // ---- self-check gate (runs once, on the first tick) ----
    bool gateDone = false;
    bool degraded = false;
    std::wstring degradeReason;
    // ---- GPU cadence + last GPU data (reused by ticks that skip the query) ----
    Clock::time_point nextGpuAt{};  // epoch -> due on the first tick
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
            // Sleep until the next tick; wake early when Stop() runs or the
            // interval changes (SetInterval must take effect immediately).
            std::unique_lock<std::mutex> lock(mu);
            for (;;) {
                if (!running) return;
                const uint32_t iv = intervalMs;
                const uint64_t gen = intervalGen;
                const auto target = Clock::now() + std::chrono::milliseconds(iv);
                cv.wait_until(lock, target,
                              [&] { return !running || intervalGen != gen; });
                if (!running) return;
                if (intervalGen != gen) continue;  // interval changed: recompute target
                break;                             // natural timeout -> next tick
            }
        }
    }

    void DoTick(SnapshotStore* store, uint64_t tickId) {
        // Self-check gate: validate the NtQSI fast path once, at startup.
        if (!gateDone) {
            gateDone = true;
            const cd::GateResult g = cd::RunSelfCheckGate();
            degraded = g.degraded;
            degradeReason = g.reason;
            if (degraded) STM_LOG_WARN("collect", Fmt(L"自校验门：{}", g.reason));
        }

        auto snap = std::make_shared<Snapshot>();
        snap->tickId = tickId;
        snap->degraded = degraded;
        snap->degradeReason = degradeReason;
        snap->caps = 0;  // CAP_NET_ETW set below only while ETW actually runs

        cd::ProcessCollector::TickOut pt;
        procCol.Collect(tickId, degraded, &pt);
        snap->procs = std::move(pt.procs);  // already ascending by pid
        snap->tickSec = pt.elapsedSec;      // delta normalization used by this tick
        if (!pt.ntsiOk && !degraded) {
            // Runtime NtQSI failure after a passed gate: honest degradation.
            snap->degraded = true;
            snap->degradeReason = L"NtQuerySystemInformation 本次调用失败，本 tick 使用兼容路径";
        }

        // --- ETW per-process net rates (phase 3, R5 #10b) -------------------
        // EtwNetCollector accumulates CUMULATIVE per-pid recv+send bytes on its
        // consumer thread; the (pid -> bytes/s) differential happens HERE in
        // the synthesis phase rather than inside ProcessCollector: the ETW
        // state lives in this Impl, so this is the smallest change and leaves
        // the ProcessCollector fast path untouched. ETW events carry pids only
        // (no createTime): a pid recycled mid-window is a phase-3 approximation.
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
                        if (now >= was) {  // counter regression (restart) -> skip tick
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

        // GPU on its own cadence; ticks in between reuse the last result.
        // gpuIntervalMs is worker-thread-only state, so no lock is needed here.
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
                            gpuIntervalMs = 4000.0;  // auto-degrade, one-way
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
// CollectService (pimpl forwarding)
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

void CollectService::SetNetEtwEnabled(bool on) {
    // Start/Stop block briefly (session control + consumer join) but never call
    // back into the service, so holding mu_ is safe; lock order is always
    // Impl::mu_ -> EtwNetCollector::mu_.
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->netEtwEnabled == on) return;
    if (on) {
        // Failure (non-admin etc.) is logged inside Start and the feature
        // stays disabled — honest, never half-enabled.
        if (impl_->netEtw.Start()) impl_->netEtwEnabled = true;
    } else {
        impl_->netEtw.Stop();
        impl_->netEtwEnabled = false;
    }
}

bool CollectService::NetEtwEnabled() const {
    // V9 P0-4: readback must be the REAL state, not just the request flag —
    // start-failure (flag never set) and a session that died after a
    // successful start (flag stale) both report false so the UI can roll the
    // toggle back honestly.
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->netEtwEnabled && impl_->netEtw.Running();
}

}  // namespace stm
