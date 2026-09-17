#include "ops/SessionState.h"
#include "core/Cfg.h"
#include "core/FsUtil.h"
#include <ctime>

namespace stm::ops {

bool SaveSession(const SessionState& s) {
    SessionState out = s;
    out.ts = static_cast<int64_t>(time(nullptr));
    Config c;
    c.SetInt(L"page", out.page);
    c.SetInt(L"selPid", out.selected.pid);
    c.SetInt(L"selCreateTime", static_cast<int64_t>(out.selected.createTime));
    c.SetString(L"sortKey", out.sortKey);
    c.SetInt(L"sortDir", out.sortDir);
    c.SetInt(L"winX", out.winX);
    c.SetInt(L"winY", out.winY);
    c.SetInt(L"winW", out.winW);
    c.SetInt(L"winH", out.winH);
    c.SetInt(L"intervalMs", out.intervalMs);
    c.SetInt(L"ts", out.ts);
    return c.Save(SessionPath());
}

bool LoadSession(SessionState* s) {
    if (!s) return false;
    Config c;
    if (!c.Load(SessionPath())) return false;
    const int64_t ts = c.GetInt(L"ts", 0);
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (ts <= 0 || now - ts > 60) return false;  // stale/corrupted -> fresh start

    s->page = static_cast<int>(c.GetInt(L"page", 0));
    s->selected.pid = static_cast<uint32_t>(c.GetInt(L"selPid", 0));
    s->selected.createTime = static_cast<uint64_t>(c.GetInt(L"selCreateTime", 0));
    s->sortKey = c.GetString(L"sortKey", L"name");
    s->sortDir = static_cast<int>(c.GetInt(L"sortDir", 0));
    s->winX = static_cast<long>(c.GetInt(L"winX", 0));
    s->winY = static_cast<long>(c.GetInt(L"winY", 0));
    s->winW = static_cast<long>(c.GetInt(L"winW", 0));
    s->winH = static_cast<long>(c.GetInt(L"winH", 0));
    s->intervalMs = static_cast<uint32_t>(c.GetInt(L"intervalMs", 1000));
    s->ts = ts;
    return true;
}

}  // namespace stm::ops
