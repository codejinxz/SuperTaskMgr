#pragma once
// Phase-3 contract: connection tables (GetExtendedTcp/UdpTable OWNER_PID).
// Architect-owned, frozen. Runs on the ops job queue (blocking call, ms-level).
// Non-admin: table works; module info unavailable (OWNER_MODULE requires elevation) —
// we do not attempt it in phase 3 and never display fake module names.
#include <cstdint>
#include <string>
#include <vector>

namespace stm {

enum class ConnProto : uint32_t { Tcp4 = 0, Tcp6, Udp4, Udp6 };

struct ConnEntry {
    ConnProto proto = ConnProto::Tcp4;
    std::wstring localAddr;   // dotted / hex-colon IPv6
    uint16_t localPort = 0;
    std::wstring remoteAddr;  // empty for UDP
    uint16_t remotePort = 0;
    uint32_t state = 0;       // MIB_TCP_STATE value (TCP only); UDP = 0
    uint32_t pid = 0;         // 0 = system-bound (bound by kernel/System)
};

// Snapshot of all TCP + UDP endpoints with owning PIDs. err on failure.
std::vector<ConnEntry> SnapshotConnections(std::wstring* err);

// MIB_TCP_STATE_* -> Chinese short label ("监听"/"已建立"/...); unknown -> hex.
std::wstring TcpStateLabel(uint32_t state);

}  // namespace stm
