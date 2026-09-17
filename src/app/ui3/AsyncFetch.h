#pragma once
// Reusable async data fetch for the phase-3 pages: page-side cache + ops job
// submission with a per-page minimum refresh interval (2 s net / 5 s services
// and drivers / 10 s sensors), so no page ever blocks the UI thread and no
// page hammers the serial ops queue every frame.
//
// Lifetime model mirrors Pages.cpp ops jobs: the producer lambda is executed
// on the JobQueue worker while capturing the shared AppContext, and results
// land in a shared reference-counted State cell. A job that outlives the app
// (Shutdown timeout) only writes into the cell it already holds — never into
// a page object.
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
        bool ok = false;        // true when err came back empty
        T data{};               // partial data is allowed even with err set
        std::wstring err;       // user-facing Chinese; empty on success
    };

    explicit AsyncFetch(double minIntervalSec)
        : st_(std::make_shared<State>()), minIntervalSec_(minIntervalSec) {}

    // UI thread. Submits a produce job when `force` is set (manual refresh:
    // bypasses the min interval — wording of refresh-button tooltips must match
    // this), or when the first fetch has not happened yet, or when the min
    // interval elapsed since the last attempt. Returns true when a job was
    // queued. While a job is in flight every call is a no-op.
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
        // Submit returns 0 when the queue is not running (teardown): undo.
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

    // Latest completed result; nullptr until the first job finished.
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
