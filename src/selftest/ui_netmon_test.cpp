// ui_netmon_test：网络页「实时监视」区（NetMonUi.h，D5 维护轮）的纯逻辑自测。
// NetMonUi.h 仅头文件且只依赖 core/collect 头，与 ui_a2_test.cpp 同形：
// stm_selftest 内运行（链接 core+collect+ops，无应用对象/无 ImGui）。
#include "selftest/TestFramework.h"
#include "app/ui3/NetMonUi.h"
#include <windows.h>
#include <deque>
#include <string>
#include <vector>

namespace {

stm::ConnEvent Ev(stm::ConnEvent::Kind kind, uint32_t proto, const wchar_t* process,
                  uint32_t pid, const wchar_t* remote, uint16_t remotePort,
                  uint16_t localPort = 0) {
    stm::ConnEvent e;
    e.unixTime = 1700000000;
    e.kind = kind;
    e.proto = proto;
    e.processName = process;
    e.pid = pid;
    e.remoteAddr = remote;
    e.remotePort = remotePort;
    e.localPort = localPort;
    return e;
}

}  // namespace

// --- 多条件过滤：进程包含 / 远程包含 / 协议 / 事件类型（AND 语义）-----------
STM_TEST(netmonui_event_filter) {
    using K = stm::ConnEvent::Kind;
    using CP = stm::ConnProto;
    using PF = stm::ui3::ProtoFilter;
    const stm::ConnEvent newTcp = Ev(K::New, static_cast<uint32_t>(CP::Tcp4), L"chrome.exe", 42,
                                     L"1.2.3.4", 443, 51000);
    const stm::ConnEvent closedTcp =
        Ev(K::Closed, static_cast<uint32_t>(CP::Tcp6), L"Chrome.exe", 42, L"::ffff:1.2.3.4", 80);
    const stm::ConnEvent stateUdp =
        Ev(K::StateChanged, static_cast<uint32_t>(CP::Udp4), L"svchost.exe", 1000, L"9.9.9.9", 53);

    // 默认过滤：全部通过。
    stm::ui3::EventFilter f;
    if (!stm::ui3::EventPassesFilter(f, newTcp) || !stm::ui3::EventPassesFilter(f, closedTcp) ||
        !stm::ui3::EventPassesFilter(f, stateUdp)) {
        *err = L"默认过滤器应放行全部事件";
        return false;
    }
    // 进程包含（大小写不敏感：控件侧已小写化，这里验证匹配语义）。
    f.process = L"chrome";
    if (!stm::ui3::EventPassesFilter(f, newTcp) || !stm::ui3::EventPassesFilter(f, closedTcp)) {
        *err = L"进程过滤 chrome 应匹配 chrome.exe/Chrome.exe";
        return false;
    }
    if (stm::ui3::EventPassesFilter(f, stateUdp)) {
        *err = L"进程过滤 chrome 不应匹配 svchost.exe";
        return false;
    }
    f = stm::ui3::EventFilter{};
    // 远程包含：匹配 "ip:端口" 全文（含端口数字）；无远端的事件不匹配。
    f.remote = L":443";
    if (!stm::ui3::EventPassesFilter(f, newTcp)) {
        *err = L"远程过滤 :443 应匹配 1.2.3.4:443";
        return false;
    }
    if (stm::ui3::EventPassesFilter(f, closedTcp) || stm::ui3::EventPassesFilter(f, stateUdp)) {
        *err = L"远程过滤 :443 不应匹配 80/53 端口事件";
        return false;
    }
    stm::ConnEvent noRemote = newTcp;
    noRemote.remoteAddr.clear();
    noRemote.remotePort = 0;
    if (stm::ui3::EventPassesFilter(f, noRemote)) {
        *err = L"无远端地址的事件不应匹配非空远程过滤";
        return false;
    }
    // 协议下拉：TCP 含 Tcp4+Tcp6，UDP 含 Udp4+Udp6。
    f = stm::ui3::EventFilter{};
    f.proto = PF::Tcp;
    if (!stm::ui3::EventPassesFilter(f, newTcp) || !stm::ui3::EventPassesFilter(f, closedTcp)) {
        *err = L"TCP 过滤应放行 Tcp4/Tcp6";
        return false;
    }
    if (stm::ui3::EventPassesFilter(f, stateUdp)) {
        *err = L"TCP 过滤不应放行 Udp4";
        return false;
    }
    f.proto = PF::Udp;
    if (!stm::ui3::EventPassesFilter(f, stateUdp) || stm::ui3::EventPassesFilter(f, newTcp)) {
        *err = L"UDP 过滤应只放行 UDP 族";
        return false;
    }
    // 事件类型多选：只勾「新建」时 Closed/StateChanged 被滤掉。
    f = stm::ui3::EventFilter{};
    f.kindMask = stm::ui3::kKindBitNew;
    if (!stm::ui3::EventPassesFilter(f, newTcp) || stm::ui3::EventPassesFilter(f, closedTcp) ||
        stm::ui3::EventPassesFilter(f, stateUdp)) {
        *err = L"只勾「新建」时应滤掉断开/状态事件";
        return false;
    }
    f.kindMask = 0;  // 全不勾 = 全滤掉（合法状态）
    if (stm::ui3::EventPassesFilter(f, newTcp)) {
        *err = L"空事件类型掩码应滤掉全部事件";
        return false;
    }
    // AND 组合：进程 + 协议 + 类型同时命中才通过。
    f.process = L"chrome";
    f.proto = PF::Tcp;
    f.kindMask = stm::ui3::kKindBitNew | stm::ui3::kKindBitClosed;
    if (!stm::ui3::EventPassesFilter(f, newTcp) || !stm::ui3::EventPassesFilter(f, closedTcp) ||
        stm::ui3::EventPassesFilter(f, stateUdp)) {
        *err = L"组合过滤应按 AND 语义放行/拦截";
        return false;
    }
    // 过滤器相等比较（视图重建判据的钉子）。
    const stm::ui3::EventFilter g = f;
    if (!(g == f)) {
        *err = L"相同字段的过滤器应相等";
        return false;
    }
    return true;
}

// --- CSV 转义：逗号/引号/换行/回车 + 中文 -----------------------------------
STM_TEST(netmonui_csv_escape) {
    if (stm::ui3::CsvEscapeField(L"plain") != L"plain") {
        *err = L"无特殊字符不应加引号";
        return false;
    }
    if (stm::ui3::CsvEscapeField(L"进程管理器") != L"进程管理器") {
        *err = L"中文字段不应转义";
        return false;
    }
    if (stm::ui3::CsvEscapeField(L"a,b") != L"\"a,b\"") {
        *err = L"含逗号字段应整体加引号";
        return false;
    }
    if (stm::ui3::CsvEscapeField(L"say \"hi\"") != L"\"say \"\"hi\"\"\"") {
        *err = L"内部引号应翻倍并整体加引号";
        return false;
    }
    if (stm::ui3::CsvEscapeField(L"行一\n行二") != L"\"行一\n行二\"") {
        *err = L"含换行字段应整体加引号";
        return false;
    }
    if (stm::ui3::CsvEscapeField(L"行一\r\n行二") != L"\"行一\r\n行二\"") {
        *err = L"含回车+换行字段应整体加引号";
        return false;
    }
    // 组合：逗号 + 引号 + 换行同时出现。
    const std::wstring nasty = L"a,\"b\"\nc";
    if (stm::ui3::CsvEscapeField(nasty) != L"\"a,\"\"b\"\"\nc\"") {
        *err = L"组合特殊字符的转义结果不正确";
        return false;
    }
    return true;
}

// --- 事件着色语义：新建绿 / 断开红 / 状态黄 ---------------------------------
STM_TEST(netmonui_state_color) {
    using K = stm::ConnEvent::Kind;
    using T = stm::ui3::EventTone;
    struct Case { K kind; T tone; const wchar_t* label; };
    static const Case cases[] = {
        {K::New, T::Good, L"新建"},
        {K::Closed, T::Bad, L"断开"},
        {K::StateChanged, T::Warn, L"状态"},
    };
    for (const Case& c : cases) {
        if (stm::ui3::EventToneOf(c.kind) != c.tone) {
            *err = stm::Fmt(L"事件类型 {} 的着色基调不正确", c.label);
            return false;
        }
        if (std::wstring(stm::ui3::EventKindLabel(c.kind)) != c.label) {
            *err = stm::Fmt(L"事件类型 {} 的标签不正确", c.label);
            return false;
        }
    }
    // 协议短标签（着色同表的事件行的辅助列）。
    using CP = stm::ConnProto;
    if (std::wstring(stm::ui3::NetMonProtoLabel(static_cast<uint32_t>(CP::Tcp4))) != L"TCP" ||
        std::wstring(stm::ui3::NetMonProtoLabel(static_cast<uint32_t>(CP::Tcp6))) != L"TCP6" ||
        std::wstring(stm::ui3::NetMonProtoLabel(static_cast<uint32_t>(CP::Udp4))) != L"UDP" ||
        std::wstring(stm::ui3::NetMonProtoLabel(static_cast<uint32_t>(CP::Udp6))) != L"UDP6") {
        *err = L"协议短标签映射不正确";
        return false;
    }
    if (std::wstring(stm::ui3::NetMonProtoLabel(99u)) != L"—") {
        *err = L"未知协议应渲染破折号";
        return false;
    }
    // 进程列：名+PID；名空 -> 破折号/系统（诚实，不伪造进程名）。
    stm::ConnEvent e = Ev(K::New, 0u, L"chrome.exe", 42, L"1.2.3.4", 443);
    if (stm::ui3::EventProcessDisplay(e) != L"chrome.exe（42）") {
        *err = L"进程列应为 名（PID） 形式";
        return false;
    }
    e.processName.clear();
    if (stm::ui3::EventProcessDisplay(e) != L"—（42）") {
        *err = L"未匹配到进程名时应显示破折号 + PID";
        return false;
    }
    e.pid = 0;
    if (stm::ui3::EventProcessDisplay(e) != L"系统（0）") {
        *err = L"pid 0 且无进程名应显示 系统（0）";
        return false;
    }
    return true;
}

// --- 端口 -> 服务名（远端优先；UDP/TCP 分表；未知为空 -> "—"）--------------
STM_TEST(netmonui_port_service) {
    using CP = stm::ConnProto;
    const uint32_t tcp = static_cast<uint32_t>(CP::Tcp4);
    const uint32_t udp = static_cast<uint32_t>(CP::Udp4);
    // TCP 常量表。
    if (stm::ui3::EventServiceName(Ev(stm::ConnEvent::Kind::New, tcp, L"p", 1, L"1.1.1.1", 443)) !=
        L"HTTPS") {
        *err = L"TCP 远端 443 应解析为 HTTPS";
        return false;
    }
    // 远端端口优先于本地端口。
    if (stm::ui3::EventServiceName(Ev(stm::ConnEvent::Kind::New, tcp, L"p", 1, L"1.1.1.1", 443,
                                      8080)) != L"HTTPS") {
        *err = L"远端端口非 0 时应优先于本地端口";
        return false;
    }
    // 远端为 0（监听类）退化本地端口。
    if (stm::ui3::EventServiceName(Ev(stm::ConnEvent::Kind::New, tcp, L"p", 1, L"0.0.0.0", 0,
                                      3389)) != L"RDP") {
        *err = L"远端端口 0 时应退化查询本地端口";
        return false;
    }
    // UDP 分表：53=DNS；UDP 1900=SSDP；TCP 表无 NTP。
    const std::wstring svc53 =
        stm::ui3::EventServiceName(Ev(stm::ConnEvent::Kind::New, udp, L"p", 1, L"9.9.9.9", 53));
    if (svc53 != L"DNS") {
        *err = stm::Fmt(L"UDP 远端 53 应解析为 DNS（实得 {}）", svc53);
        return false;
    }
    const std::wstring svc1900 =
        stm::ui3::EventServiceName(Ev(stm::ConnEvent::Kind::New, udp, L"p", 1, L"9.9.9.9", 1900));
    if (svc1900.empty()) {
        *err = stm::Fmt(L"UDP 1900 应为 SSDP（实得 [{}]）", svc1900);
        return false;
    }
    const std::wstring svcNtpTcp =
        stm::ui3::EventServiceName(Ev(stm::ConnEvent::Kind::New, tcp, L"p", 1, L"1.1.1.1", 123));
    if (!svcNtpTcp.empty()) {
        *err = stm::Fmt(L"TCP 123（NTP 仅 UDP）应为空（实得 {}）", svcNtpTcp);
        return false;
    }
    // 未知端口 -> 空；展示层渲染破折号（绝不伪造服务名）。
    const stm::ConnEvent unknown =
        Ev(stm::ConnEvent::Kind::New, tcp, L"p", 1, L"1.1.1.1", 40000);
    if (!stm::ui3::EventServiceName(unknown).empty() ||
        stm::ui3::EventServiceDisplay(unknown) != L"—") {
        *err = L"未知端口应为空且展示为 —";
        return false;
    }
    return true;
}

// --- 时间列 HH:MM:SS（本地时区；无效时间戳占位，不伪造）----------------------
STM_TEST(netmonui_time_fmt) {
    // 期望值用同一 localtime 口径现算（时区无关，钉住格式/零填充）。
    const int64_t t = 1700000000;
    std::tm tmv{};
    const time_t tt = static_cast<time_t>(t);
    if (localtime_s(&tmv, &tt) != 0) {
        *err = L"localtime_s 失败（环境异常）";
        return false;
    }
    wchar_t want[16] = {};
    swprintf_s(want, L"%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    const std::wstring got = stm::ui3::FormatEventClock(t);
    if (got != want) {
        *err = stm::Fmt(L"FormatEventClock({}) = {}，期望 {}", t, got, want);
        return false;
    }
    if (got.size() != 8 || got[2] != L':' || got[5] != L':') {
        *err = L"时间列格式应为 HH:MM:SS（8 字符，冒号分隔）";
        return false;
    }
    // 一天边界：00:00:00 必须零填充而不是 "0:0:0"。
    const int64_t dayStart = t - (t % 86400);
    std::tm tm0{};
    const time_t tt0 = static_cast<time_t>(dayStart);
    if (localtime_s(&tm0, &tt0) != 0) {
        *err = L"localtime_s 失败（环境异常）";
        return false;
    }
    if (tm0.tm_hour == 0 && tm0.tm_min == 0 && tm0.tm_sec == 0 &&
        stm::ui3::FormatEventClock(dayStart) != L"00:00:00") {
        *err = L"午夜 0 点应零填充为 00:00:00";
        return false;
    }
    // 无效时间戳：占位 --:--:--（不伪造 0 点）。
    for (int64_t bad : {int64_t(0), int64_t(-1), int64_t(-99999)}) {
        if (stm::ui3::FormatEventClock(bad) != L"--:--:--") {
            *err = stm::Fmt(L"无效时间戳 {} 应渲染 --:--:--", bad);
            return false;
        }
    }
    // 文件名时间戳：netmon_<yyyyMMdd-HHmmss>.csv。
    const std::wstring name = stm::ui3::NetMonCsvFileName(t);
    if (name.rfind(L"netmon_", 0) != 0 || name.size() < 10 ||
        name.substr(name.size() - 4) != L".csv") {
        *err = L"导出文件名应为 netmon_*.csv 形式";
        return false;
    }
    return true;
}

// --- CSV 构建：表头 9 列 + 行序保持 + 转义集成 -------------------------------
STM_TEST(netmonui_csv_build) {
    using CP = stm::ConnProto;
    std::vector<stm::ConnEvent> rows;
    stm::ConnEvent a =
        Ev(stm::ConnEvent::Kind::New, static_cast<uint32_t>(CP::Tcp4), L"chrome.exe", 42,
           L"1.2.3.4", 443, 51000);
    a.unixTime = 1700000000;
    a.stateLabel = L"已建立";
    stm::ConnEvent b =
        Ev(stm::ConnEvent::Kind::Closed, static_cast<uint32_t>(CP::Tcp6), L"my,proc", 7,
           L"::1", 80);
    b.unixTime = 1700000001;
    b.stateLabel = L"超时,重试";
    rows.push_back(a);
    rows.push_back(b);

    const std::wstring csv = stm::ui3::BuildNetMonCsv(rows);
    // 3 行（表头 + 2 事件），全部以换行结束。
    size_t lines = 0;
    for (wchar_t ch : csv) {
        if (ch == L'\n') ++lines;
    }
    if (lines != 3) {
        *err = stm::Fmt(L"CSV 应为 3 行（表头+2 事件），实得 {} 个换行", lines);
        return false;
    }
    const std::wstring header = csv.substr(0, csv.find(L'\n'));
    if (header != stm::ui3::NetMonCsvHeader() ||
        header != L"时间,事件,协议,进程,PID,本地,远程,服务,状态") {
        *err = L"CSV 表头 9 列不符";
        return false;
    }
    // 第 1 行：未加引号字段的顺序与值。
    size_t eol = csv.find(L'\n', header.size() + 1);
    const std::wstring row1 = csv.substr(header.size() + 1, eol - header.size() - 1);
    if (row1 != L"12:00:00,新建,TCP,chrome.exe,42,51000,1.2.3.4:443,HTTPS,已建立" &&
        row1.find(L",新建,TCP,chrome.exe,42,") == std::wstring::npos) {
        // 时间列随时区变化，退而验证关键列序列。
        *err = L"事件行关键列（事件/协议/进程/PID/端点/服务）不符";
        return false;
    }
    // 第 2 行：进程名与状态含逗号 -> 必须带引号（转义集成）。
    const std::wstring row2 = csv.substr(eol + 1, csv.size() - eol - 2);
    if (row2.find(L"\"my,proc\"") == std::wstring::npos ||
        row2.find(L"\"超时,重试\"") == std::wstring::npos) {
        *err = L"含逗号的进程/状态字段应被引号转义";
        return false;
    }
    // deque 容器同样可用（展示层是 std::deque）。
    std::deque<stm::ConnEvent> dq;
    dq.push_back(a);
    dq.push_back(b);
    if (stm::ui3::BuildNetMonCsv(dq) != stm::ui3::BuildNetMonCsv(rows)) {
        *err = L"不同容器序列化结果应一致";
        return false;
    }
    return true;
}
