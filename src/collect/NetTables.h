#pragma once
// 第 3 阶段契约：连接表（GetExtendedTcp/UdpTable OWNER_PID）。
// 归架构所有，已冻结。在 ops 任务队列上运行（阻塞调用，毫秒级）。
// 非管理员：表可用；模块信息不可得（OWNER_MODULE 需要提权）——
// 第 3 阶段不尝试获取，也绝不显示伪造的模块名。
#include <cstdint>
#include <string>
#include <vector>

namespace stm {

enum class ConnProto : uint32_t { Tcp4 = 0, Tcp6, Udp4, Udp6 };

struct ConnEntry {
    ConnProto proto = ConnProto::Tcp4;
    std::wstring localAddr;   // 点分 IPv4 / 冒号十六进制 IPv6
    uint16_t localPort = 0;
    std::wstring remoteAddr;  // UDP 为空
    uint16_t remotePort = 0;
    uint32_t state = 0;       // MIB_TCP_STATE 值（仅 TCP）；UDP = 0
    uint32_t pid = 0;         // 0 = 系统侧持有（由内核/System 绑定）
};

// 全部 TCP + UDP 端点及其持有 PID 的快照。失败时设置 err。
std::vector<ConnEntry> SnapshotConnections(std::wstring* err);

// MIB_TCP_STATE_* -> Chinese short label ("监听"/"已建立"/...); unknown -> hex.
std::wstring TcpStateLabel(uint32_t state);

// V32-P2-1：启动清扫属主已死的本应用 ETW 会话（崩溃残留，每个约 4MB 非分页池）。
// 按当前进程列表逐 pid 尝试停止三个已知命名模式；存活属主的会话不碰（多实例
// 互不影响）。返回成功停止的残留会话数。在主窗口创建前调用一次即可。
int SweepStaleEtwSessions();

}  // namespace stm
