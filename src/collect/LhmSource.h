#pragma once
// F3 契约新增（2026-09-18，已向架构登记）：可选的
// LibreHardwareMonitor（LHM）桥接，默认关闭。LHM 自带内核驱动栈；
// 我们绝不分发或加载任何驱动（红线）。当用户自行运行 LHM
// 并启用其远程 web 服务器（默认 http://127.0.0.1:8085/data.json）时，
// 本模块以仅限本机的 HTTP 客户端身份轮询它：
//   - WinHTTP 动态绑定（不新增链接依赖）；
//   - 仅回环地址，1s 超时，同步一次性调用；
//   - 绝不创建常驻线程（轮询按调用方需求进行）；
//   - 每条映射读数都带 ［LHM］ 标记，并复用 Sensors.h 的
//     四态诚实模型（无法解析的值 => NoHardware，绝不给 0）。
// Connection failure => false + *err (UI shows "未检测到 LibreHardwareMonitor
// 数据源"); options are process-global and thread-safe.
#include <cstdint>
#include <string>
#include <vector>

#include "collect/Sensors.h"  // SensorReading

namespace stm {

struct LhmOptions {
    bool enabled = false;              // 默认关：用户不选择就不产生网络流量
    uint16_t port = 8085;              // LHM 远程 web 服务器默认端口
    std::wstring host = L"127.0.0.1";  // 仅回环；PollLhm 内部会再次强制
};

void SetLhmOptions(const LhmOptions& options);  // 线程安全
LhmOptions GetLhmOptions();                     // 线程安全

// 对 <host>:<port>/data.json 的一次同步轮询。绝不创建线程，
// 阻塞不超过约 1s 的 HTTP 超时。成功时用 SensorReading 条目填充 *out
//（label = LHM 节点路径，value/unit 解析自 Value 文本，
// 每个 label 都带 ［LHM］ 后缀）。失败时返回 false，
// 设置 *err 并保持 *out 为空。
bool PollLhm(std::vector<SensorReading>* out, std::wstring* err);

// 仅解析（无网络）：面向 LHM data.json 树的最小递归 JSON 解析
//（嵌套 Text/Value/Sensor/Hardware 节点；字符串含转义、数字、
// 布尔、null）。为 selftest 导出；输入畸形时返回 false。
bool ParseLhmJson(const std::string& utf8, std::vector<SensorReading>* out);

}  // namespace stm
