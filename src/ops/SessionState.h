#pragma once
// 提权重启的会话交接（架构第 7 节）。契约头——归架构所有。
// 旧实例：SaveSession -> 退出（互斥体随进程拆除释放）。
// 新实例：CreateMutex（至多等 1000ms）-> LoadSession（ts<60s 保护）。
#include <string>
#include "core/ProcData.h"

namespace stm {
namespace ops {

struct SessionState {
    int page = 0;
    ProcKey selected;
    std::wstring sortKey = L"name";   // 列 id
    int sortDir = 0;                  // 0 升序，1 降序
    long winX = 0, winY = 0, winW = 0, winH = 0;
    uint32_t intervalMs = 1000;
    int64_t ts = 0;                   // Unix 秒，由 SaveSession 写入
};

// 原子写入（临时文件 + MoveFileExW REPLACE_EXISTING）。内部设置 ts。
bool SaveSession(const SessionState& s);
// 缺失 / 无法解析 / 超过 60s 时返回 false（此时按全新启动处理）。
bool LoadSession(SessionState* out);

}  // namespace ops
}  // namespace stm
