// collect3: phase-3 additions — connection tables (contract NetTables.h),
// sensor honesty (contract Sensors.h) and the ETW per-pid rate toggle lifecycle
// (CollectService, default OFF; admin-only feature, selftest covers both
// permission branches). White-box include of the internal CollectDetail.h is
// deliberate: the ETW event-rate probe needs EtwNetCollector directly, because
// the frozen CollectService contract deliberately exposes no event counter.
#include "selftest/TestFramework.h"
#include "collect/CollectDetail.h"
#include "collect/CollectService.h"
#include "collect/NetTables.h"
#include "collect/Sensors.h"
#include "core/Privilege.h"
#include "core/Str.h"
#include <windows.h>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
STM_TEST(net_tables_ok) {
    std::wstring terr;
    const std::vector<stm::ConnEntry> conns = stm::SnapshotConnections(&terr);
    if (!terr.empty()) {
        *err = L"SnapshotConnections err：" + terr;
        return false;
    }
    if (conns.empty()) {
        *err = L"连接表为空（系统必有监听/绑定）";
        return false;
    }
    bool hasTcp = false, hasUdp = false;
    for (const stm::ConnEntry& c : conns) {
        if (c.proto != stm::ConnProto::Tcp4 && c.proto != stm::ConnProto::Tcp6 &&
            c.proto != stm::ConnProto::Udp4 && c.proto != stm::ConnProto::Udp6) {
            *err = L"出现非法 proto 值";
            return false;
        }
        if (c.localPort == 0) {
            *err = stm::Fmt(L"localPort=0（{}:{} pid={}）", c.localAddr, c.localPort, c.pid);
            return false;
        }
        if (c.localAddr.empty()) {
            *err = L"localAddr 为空";
            return false;
        }
        const bool tcp = c.proto == stm::ConnProto::Tcp4 || c.proto == stm::ConnProto::Tcp6;
        if (tcp) {
            hasTcp = true;
            if (c.state == 0) {
                *err = L"TCP 条目 state=0";
                return false;
            }
            // Label must be a known Chinese state or non-empty hex fallback.
            if (stm::TcpStateLabel(c.state).empty()) {
                *err = L"TcpStateLabel 返回空";
                return false;
            }
        } else {
            hasUdp = true;
            if (!c.remoteAddr.empty() || c.remotePort != 0 || c.state != 0) {
                *err = L"UDP 条目不应有远端/状态";
                return false;
            }
        }
    }
    if (!hasTcp || !hasUdp) {
        *err = L"proto 分布不完整（缺 TCP 或 UDP）";
        return false;
    }
    if (stm::TcpStateLabel(2) != L"监听" || stm::TcpStateLabel(3) != L"同步发送" ||
        stm::TcpStateLabel(5) != L"已建立" || stm::TcpStateLabel(11) != L"时间等待" ||
        stm::TcpStateLabel(12) != L"删除" || stm::TcpStateLabel(1) != L"关闭") {
        *err = L"TcpStateLabel 常用态映射错误";
        return false;
    }
    if (stm::TcpStateLabel(0x99).empty()) {
        *err = L"未知 state 的 hex 回退为空";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
STM_TEST(sensors_honest) {
    std::wstring serr;
    const stm::SensorSnapshot snap = stm::ReadSensors(&serr);
    using State = stm::SensorReading::State;

    // CPU: at least one frequency reading Ok with value > 0 (documented API,
    // no admin needed); temperature readings follow the trichotomy.
    bool freqOk = false;
    for (const stm::SensorReading& r : snap.cpu) {
        if (r.unit == L"MHz") {
            if (r.state != State::Ok || r.value <= 0.0) {
                *err = stm::Fmt(L"CPU 频率读数非法：{} state={} value={}", r.label,
                           static_cast<int>(r.state), r.value);
                return false;
            }
            freqOk = true;
        } else if (r.unit == L"°C") {
            if (r.state == State::Ok && r.value <= 0.0) {
                *err = L"温度出现 Ok 且 value<=0（假数据）";
                return false;
            }
            if (r.state != State::Ok && r.state != State::NeedAdmin &&
                r.state != State::NoHardware) {
                *err = L"温度状态超出诚实三分法集合";
                return false;
            }
        }
    }
    if (!freqOk) {
        *err = L"无 Ok 且 value>0 的 CPU 频率读数";
        return false;
    }

    // GPU: only Ok readings may appear, and Ok requires value > 0.
    for (const stm::SensorReading& r : snap.gpu) {
        if (r.state != State::Ok || r.value <= 0.0) {
            *err = stm::Fmt(L"GPU 读数非法：{}", r.label);
            return false;
        }
    }

    // Fans: exactly the NeedDriver honesty state, no fake rpm.
    if (snap.fans.empty()) {
        *err = L"fans 缺少 NeedDriver 读数";
        return false;
    }
    for (const stm::SensorReading& r : snap.fans) {
        if (r.state != State::NeedDriver) {
            *err = L"fans 出现非 NeedDriver 读数";
            return false;
        }
    }

    // Disks: real model line (never a blank placeholder), health always a real
    // string; Ok temperature requires tempC > 0 (V10: 绝无 Ok+0 温度).
    for (const stm::DiskHealth& d : snap.disks) {
        if (d.model.empty()) {
            *err = L"磁盘条目 model 为空（应回退 PhysicalDriveN 或 WMI 名称）";
            return false;
        }
        if (d.health.empty()) {
            *err = stm::Fmt(L"{} 的 health 为空", d.model);
            return false;
        }
        if (d.tempState == State::Ok && d.tempC <= 0.0) {
            *err = stm::Fmt(L"{} 出现 Ok 且 tempC<=0", d.model);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
STM_TEST(etw_toggle_lifecycle) {
    const bool admin = stm::IsProcessElevated();
    const std::wstring sessionName = stm::Fmt(L"SuperTaskMgr-Net-{}", ::GetCurrentProcessId());

    // --- part 1: white-box event-rate probe (default-off feature, direct) ---
    {
        stm::cd::EtwNetCollector probe;
        if (probe.Start()) {
            ::Sleep(2000);
            const double evPerSec = static_cast<double>(probe.TotalEvents()) / 2.0;
            printf("[etw] 事件率 ≈ %.0f 事件/秒（缓冲 64x64KB，仅 recv/send 事件）\n", evPerSec);
            probe.Stop();
        } else if (admin) {
            *err = L"管理员环境下 EtwNetCollector 启动失败";
            return false;
        } else {
            printf("[etw] 非管理员：StartTraceW 拒绝，保持禁用（预期）\n");
        }
    }

    // --- part 2: CollectService wiring --------------------------------------
    stm::CollectService svc;
    if (!svc.Start(500)) {
        *err = L"CollectService::Start 失败";
        return false;
    }
    if (svc.NetEtwEnabled()) {
        *err = L"ETW 默认应为关闭";
        return false;
    }

    svc.SetNetEtwEnabled(true);
    if (!admin) {
        // StartTraceW fails with access denied: stays disabled, no crash, caps
        // clean and netBytesPerSec untouched (kUnavail).
        if (svc.NetEtwEnabled()) {
            *err = L"非管理员竟然启用成功（环境与预期不符）";
            return false;
        }
        bool reached = false;
        for (int waited = 0; waited < 4000 && !reached; waited += 50) {
            ::Sleep(50);
            if (svc.Store().Get()->tickId >= 2) reached = true;
        }
        if (!reached) {
            *err = L"未产出 2 个 tick";
            return false;
        }
        const auto s = svc.Store().Get();
        if ((s->caps & stm::CAP_NET_ETW) != 0) {
            *err = L"未启用时 caps 出现 CAP_NET_ETW";
            return false;
        }
        for (const stm::ProcInfo& p : s->procs) {
            if (p.netBytesPerSec == p.netBytesPerSec) {  // !isnan: must stay kUnavail
                *err = L"未启用时 netBytesPerSec 非 kUnavail";
                return false;
            }
        }
        svc.Stop();
        return true;
    }

    // Admin branch: enable -> 2s -> disable -> re-enable -> destructor cleanup.
    if (!svc.NetEtwEnabled()) {
        *err = L"管理员下 SetNetEtwEnabled(true) 未生效";
        return false;
    }
    bool reached = false;
    for (int waited = 0; waited < 6000 && !reached; waited += 100) {
        ::Sleep(100);
        if (svc.Store().Get()->tickId >= 4) reached = true;
    }
    if (!reached) {
        *err = L"启用后未产出 4 个 tick";
        return false;
    }
    const auto s = svc.Store().Get();
    if ((s->caps & stm::CAP_NET_ETW) == 0) {
        *err = L"启用后 caps 未置 CAP_NET_ETW";
        return false;
    }
    size_t withNet = 0;
    for (const stm::ProcInfo& p : s->procs) {
        if (p.netBytesPerSec == p.netBytesPerSec) ++withNet;  // finite = real delta
    }
    printf("[etw] 启用后含 netBytesPerSec 的进程数：%zu（差分第二 tick 起有效）\n", withNet);

    svc.SetNetEtwEnabled(false);
    if (svc.NetEtwEnabled()) {
        *err = L"disable 后仍显示启用";
        return false;
    }
    svc.SetNetEtwEnabled(true);
    if (!svc.NetEtwEnabled()) {
        *err = L"再次 enable 失败";
        return false;
    }
    svc.Stop();

    // Enabled-at-destruction cleanup: the service must stop the session and
    // join the consumer in its destructor, leaving no orphan session behind.
    {
        stm::CollectService svc2;
        if (!svc2.Start(500)) {
            *err = L"svc2 Start 失败";
            return false;
        }
        svc2.SetNetEtwEnabled(true);
        if (!svc2.NetEtwEnabled()) {
            *err = L"svc2 启用失败";
            return false;
        }
        ::Sleep(2200);  // let events flow through the tick path
    }  // destructor: Stop + ETW teardown; must not hang or crash
    if (stm::cd::EtwNetCollector::SessionExists(sessionName.c_str())) {
        *err = L"析构后残留 ETW 会话（孤儿会话）：" + sessionName;
        return false;
    }
    return true;
}
