#pragma once
// F4#7 性能日志 CSV：表头/行生成（纯函数）+ UI 线程记录器。
//
// 口径（诚实数据原则）：
//  - 1 Hz 每采集 tick 追加一行，与性能页环形历史同一摄取点（AppendHistory）。
//  - 不可用指标（NaN / 无数据）写空单元格 —— 绝不写 0 冒充数据。
//  - 仅系统级指标，不含任何进程级数据（规避隐私面，F4#8 裁决同款）。
//  - UI 线程 ofstream 小写入可接受（1 Hz、每行 <300 字节）；每行 flush，
//    崩溃后已写数据仍可读。
// 表头生成/行生成均为纯函数（csv_header_roundtrip 自测锁定列数一致）。
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>
#include "core/FsUtil.h"
#include "core/ProcData.h"
#include "core/Str.h"

namespace stm {
namespace ui3 {

// 列：时间, CPU%, 核心0%..核心N-1%, 内存可用B, 内存提交B, 磁盘读B/s, 磁盘写B/s,
//     网络收B/s, 网络发B/s, GPU利用率%, 磁盘队列
inline std::wstring PerfCsvHeader(size_t coreCount) {
    std::wstring h = L"时间,CPU%";
    for (size_t i = 0; i < coreCount; ++i) h += Fmt(L",核心{}%", i);
    h += L",内存可用B,内存提交B,磁盘读B/s,磁盘写B/s,网络收B/s,网络发B/s,GPU利用率%,磁盘队列";
    return h;
}

namespace csv_detail {
inline void AppendCell(std::wstring& row, const std::wstring& v) {
    row += L",";
    row += v;
}
inline void AppendDouble(std::wstring& row, double v, bool percent) {
    if (v != v) {          // NaN = 不可达/未启用，写空单元格
        AppendCell(row, std::wstring());
        return;
    }
    AppendCell(row, percent ? Fmt(L"{:.2f}", v) : Fmt(L"{:.0f}", v));
}
inline void AppendU64(std::wstring& row, uint64_t v) {
    AppendCell(row, Fmt(L"{}", v));  // 0 是合法读数（快照契约：uint64 恒有效）
}
}  // namespace csv_detail

// 行值与表头列数严格一致（localTime 由调用方生成，纯函数可测）。
inline std::wstring PerfCsvRow(const Snapshot& s, const std::wstring& localTime) {
    using namespace csv_detail;
    std::wstring row = localTime;  // 第 1 列无前置逗号
    AppendDouble(row, s.sys.cpuTotalPercent, true);
    const size_t cores = s.sys.perCorePercent.size();
    for (size_t i = 0; i < cores; ++i) AppendDouble(row, s.sys.perCorePercent[i], true);
    AppendU64(row, s.sys.physAvail);
    AppendU64(row, s.sys.commitTotal);
    AppendDouble(row, s.sys.diskReadBps, false);
    AppendDouble(row, s.sys.diskWriteBps, false);
    AppendDouble(row, s.sys.netRecvBps, false);
    AppendDouble(row, s.sys.netSendBps, false);
    // GPU 利用率取首个适配器（多卡汇总口径在表头注明 "GPU利用率%"，诚实首列）
    if (!s.sys.gpus.empty()) {
        AppendDouble(row, s.sys.gpus[0].utilPercent, true);
    } else {
        AppendCell(row, std::wstring());
    }
    // Phase C：磁盘队列深度（PDH Current Disk Queue Length 合计）；
    // 本机无此计数器 -> kUnavail -> 空单元格（绝不写 0 冒充数据）。
    AppendDouble(row, s.sys.diskQueueDepth, false);
    return row;
}

// %LOCALAPPDATA%\SuperTaskMgr\captures
inline std::wstring PerfCsvDefaultDir() { return LocalAppDataRoot() + L"\\captures"; }

// unix 秒 -> 本地时间 "yyyy-MM-dd HH:mm:ss"（与 CrashUi.h 同款口径，独立实现避免跨头耦合）。
inline std::wstring PerfCsvLocalTime(int64_t unixSec) {
    if (unixSec <= 0) return L"—";
    const std::time_t t = static_cast<time_t>(unixSec);
    std::tm tmv{};
    if (localtime_s(&tmv, &t) != 0) return L"—";
    wchar_t buf[32] = {};
    if (swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                   tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec) <= 0) {
        return L"—";
    }
    return buf;
}

// ---------------------------------------------------------------------------
// 记录器：UI 线程使用（Start/Append/Stop 都在主线程，无需加锁）。
// ---------------------------------------------------------------------------
class PerfCsvRecorder {
public:
    bool Start(const std::wstring& dir, size_t coreCount, std::wstring* err) {
        Stop();
        const std::wstring target = dir.empty() ? PerfCsvDefaultDir() : dir;
        // EnsureDir 契约：成功返回目录路径，失败返回空串。
        if (EnsureDir(target).empty()) {
            if (err != nullptr) *err = Fmt(L"创建目录失败：{}", target);
            return false;
        }
        std::wstring name;
        {
            const std::time_t t = std::time(nullptr);
            std::tm tmv{};
            if (localtime_s(&tmv, &t) != 0) tmv = std::tm{};
            wchar_t buf[40] = {};
            swprintf_s(buf, L"perf_%04d%02d%02d-%02d%02d%02d.csv", tmv.tm_year + 1900,
                       tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            name = buf;
        }
        path_ = target + L"\\" + name;
        f_.open(path_.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f_.is_open()) {
            if (err != nullptr) *err = L"无法创建文件（被占用或无写入权限）";
            path_.clear();
            return false;
        }
        cores_ = coreCount;
        f_ << "\xEF\xBB\xBF";  // UTF-8 BOM：Excel 直接双击可读
        const std::string header = WideToUtf8(PerfCsvHeader(cores_));
        f_ << header << "\n";
        if (!f_.good()) {
            if (err != nullptr) *err = L"写入表头失败（磁盘错误）";
            f_.close();
            path_.clear();
            return false;
        }
        f_.flush();
        if (err != nullptr) err->clear();
        return true;
    }

    // 每个 tick 调用；时间戳取快照自身（采集线程产出，非渲染帧时间）。
    // 返回 false = 写入失败（磁盘满/流错误，V15-P1）：记录器已自行 Stop，
    // 调用方必须如实 toast（绝不静默丢行继续显示"记录中"）。
    bool Append(const Snapshot& s) {
        if (!f_.is_open()) return true;
        f_ << WideToUtf8(PerfCsvRow(s, PerfCsvLocalTime(s.timestamp))) << "\n";
        f_.flush();
        if (!f_.good()) {  // failbit/badbit：磁盘满、配额、文件被夺走等
            Stop();
            return false;
        }
        return true;
    }

    void Stop() {
        if (f_.is_open()) {
            f_.flush();
            f_.close();
        }
    }

    bool Active() const { return const_cast<PerfCsvRecorder*>(this)->f_.is_open(); }
    const std::wstring& Path() const { return path_; }

private:
    std::ofstream f_;
    std::wstring path_;
    size_t cores_ = 0;
};

// 全局记录器（单 UI 线程访问；Pages.cpp 摄取点与性能页控件共用）。
inline PerfCsvRecorder& SharedPerfCsv() {
    static PerfCsvRecorder r;
    return r;
}

}  // namespace ui3
}  // namespace stm
