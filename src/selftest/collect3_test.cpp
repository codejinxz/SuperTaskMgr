// collect3：第 3 阶段新增——连接表（契约 NetTables.h）、
// 传感器诚实性（契约 Sensors.h）与 ETW 每 pid 速率开关生命周期
//（CollectService，默认关；仅管理员功能，selftest 覆盖两种
// 权限分支）。刻意白盒包含内部 CollectDetail.h：ETW 事件速率探测需要
// 直接使用 EtwNetCollector，因为冻结的 CollectService 契约
// 刻意不暴露事件计数器。
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
            // 标签必须是已知中文状态或非空十六进制兜底。
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

    // CPU：至少一条频率读数 Ok 且值 > 0（有文档 API，无需管理员）；
    // 温度读数遵循三分类。
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

    // GPU：只允许出现 Ok 读数，且 Ok 要求值 > 0。
    for (const stm::SensorReading& r : snap.gpu) {
        if (r.state != State::Ok || r.value <= 0.0) {
            *err = stm::Fmt(L"GPU 读数非法：{}", r.label);
            return false;
        }
    }

    // 风扇：恰好是 NeedDriver 诚实状态，无假 rpm。
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

    // 磁盘：真实型号行（绝无空白占位），健康始终是真实
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

    // --- 第 1 部分：白盒事件速率探测（默认关功能，直接调用）---
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

    // --- 第 2 部分：CollectService 接线 --------------------------------------
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
        // StartTraceW 以拒绝访问失败：保持禁用、不崩溃、caps 干净、
        // netBytesPerSec 不受影响（kUnavail）。
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
            if (p.netBytesPerSec == p.netBytesPerSec) {  // 非NaN：必须保持 kUnavail
                *err = L"未启用时 netBytesPerSec 非 kUnavail";
                return false;
            }
        }
        svc.Stop();
        return true;
    }

    // 管理员分支：启用 -> 2s -> 禁用 -> 再启用 -> 析构清理。
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
        if (p.netBytesPerSec == p.netBytesPerSec) ++withNet;  // 有限值 = 真实差值
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

    // 启用状态下析构的清理：服务必须在析构中停止会话并
    // join 消费线程，不留孤儿会话。
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
        ::Sleep(2200);  // 让事件流经 tick 路径
    }  // 析构：Stop + ETW 拆除；不得挂起或崩溃
    if (stm::cd::EtwNetCollector::SessionExists(sessionName.c_str())) {
        *err = L"析构后残留 ETW 会话（孤儿会话）：" + sessionName;
        return false;
    }
    return true;
}
