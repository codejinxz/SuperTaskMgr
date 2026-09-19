// sensors_test：F3 传感器层扩展用例（契约 Sensors.h + LhmSource）。
// 诚实性重点：每个新组遵守四态模型（Ok/NeedAdmin/
// NeedDriver/NoHardware），绝不显示伪造的 "Ok + 0" 值；LHM
// 桥接默认关闭，其迷你 JSON 解析器离线校验。
#include "selftest/TestFramework.h"
#include "collect/LhmSource.h"
#include "collect/Sensors.h"
#include "core/Str.h"
#include <string>
#include <vector>

namespace {

using State = stm::SensorReading::State;

bool IsKnownState(State s) {
    return s == State::Ok || s == State::NeedAdmin || s == State::NeedDriver ||
           s == State::NoHardware;
}

}  // namespace

// ---------------------------------------------------------------------------
// sensors_extended_honest：新的 F3 组存在且保持诚实。无电池的机器上
// 电池为 NoHardware；网络有 >=1 个 Ok 适配器；内存为 Ok；
// 磁盘温度为 Ok/NeedAdmin/NoHardware，任何地方都无 "Ok + 0"。
// ---------------------------------------------------------------------------
STM_TEST(sensors_extended_honest) {
    std::wstring serr;
    const stm::SensorSnapshot snap = stm::ReadSensors(&serr);

    // --- cpuCores：非空；Ok 频率读数 > 0；Ok 利用率在 [0,100]。
    if (snap.cpuCores.empty()) {
        *err = L"cpuCores 为空（应至少包含每核频率读数）";
        return false;
    }
    bool coreFreqOk = false;
    for (const stm::SensorReading& r : snap.cpuCores) {
        if (!IsKnownState(r.state)) {
            *err = L"cpuCores 出现未知状态";
            return false;
        }
        if (r.unit == L"MHz") {
            if (r.state == State::Ok && r.value <= 0.0) {
                *err = L"CPU 频率出现 Ok 且 <=0（假数据）";
                return false;
            }
            if (r.state == State::Ok) coreFreqOk = true;
        } else if (r.unit == L"%") {
            if (r.state == State::Ok && (r.value < 0.0 || r.value > 100.0)) {
                *err = stm::Fmt(L"每核占用率超界：{} = {}", r.label, r.value);
                return false;
            }
        }
    }
    if (!coreFreqOk) {
        *err = L"cpuCores 缺少 Ok 的频率读数";
        return false;
    }

    // --- gpus：只有已知状态；Ok 摄氏读数必须合理为正。
    for (const stm::SensorReading& r : snap.gpus) {
        if (!IsKnownState(r.state)) {
            *err = L"gpus 出现未知状态";
            return false;
        }
        if (r.state == State::Ok && r.unit == L"°C" && r.value <= 0.0) {
            *err = L"GPU 温度出现 Ok 且 <=0（假数据）";
            return false;
        }
        if (r.state == State::Ok && r.unit == L"%" && (r.value < 0.0 || r.value > 100.0)) {
            *err = L"GPU 引擎占用率超界";
            return false;
        }
        if (r.state == State::Ok && r.unit == L"GiB" && r.value < 0.0) {
            *err = L"GPU 显存出现 Ok 且 <0";
            return false;
        }
    }

    // --- network：至少一条 Ok 适配器读数（本机在线）。
    bool netOk = false;
    for (const stm::SensorReading& r : snap.network) {
        if (!IsKnownState(r.state)) {
            *err = L"network 出现未知状态";
            return false;
        }
        if (r.state == State::Ok && (r.unit == L"B/s" || r.unit == L"Mbps") && r.value >= 0.0) {
            netOk = true;
        }
        if (r.state == State::Ok && r.value < 0.0) {
            *err = L"network 出现 Ok 且 <0";
            return false;
        }
    }
    if (!netOk) {
        *err = L"network 无 Ok 的适配器读数（至少应有一个在线网卡）";
        return false;
    }

    // --- battery：只有已知状态；无电池的机器必须给出
    //     NoHardware 条目而不是伪造百分比。
    if (snap.battery.empty()) {
        *err = L"battery 为空（有电池应给出 Ok 读数，无电池应给出 NoHardware 条目）";
        return false;
    }
    for (const stm::SensorReading& r : snap.battery) {
        if (!IsKnownState(r.state)) {
            *err = L"battery 出现未知状态";
            return false;
        }
        if (r.state == State::NeedDriver) {
            *err = L"battery 不应出现 NeedDriver（电池状态是用户态可判定的）";
            return false;
        }
        if (r.state == State::Ok && r.unit == L"%" && (r.value < 0.0 || r.value > 100.0)) {
            *err = L"电池电量超界";
            return false;
        }
    }

    // --- memory：物理用量必须存在且为 Ok、百分比合理。
    bool memOk = false;
    for (const stm::SensorReading& r : snap.memory) {
        if (!IsKnownState(r.state)) {
            *err = L"memory 出现未知状态";
            return false;
        }
        if (r.state == State::Ok && r.label == L"物理内存占用" && r.unit == L"%" &&
            r.value >= 0.0 && r.value <= 100.0) {
            memOk = true;
        }
    }
    if (!memOk) {
        *err = L"memory 缺少 Ok 的物理内存占用读数";
        return false;
    }

    // --- disks：温度遵循诚实三分类且绝不 Ok + 0；
    //     F3 细节字段只在真实 NVMe 日志读取后出现。
    if (snap.disks.empty()) {
        *err = L"disks 为空（本机至少应枚举出一块物理盘）";
        return false;
    }
    for (const stm::DiskHealth& d : snap.disks) {
        if (d.tempState != State::Ok && d.tempState != State::NeedAdmin &&
            d.tempState != State::NoHardware) {
            *err = L"磁盘温度状态超出 Ok/NeedAdmin/NoHardware 三态";
            return false;
        }
        if (d.tempState == State::Ok && d.tempC <= 0.0) {
            *err = L"磁盘温度出现 Ok 且 <=0（绝无 Ok+0）";
            return false;
        }
        if (d.tempState != State::Ok && d.tempC != 0.0) {
            *err = L"磁盘温度非 Ok 却带有非 0 数值";
            return false;
        }
        if (!d.critWarnValid &&
            (d.critWarnBits != 0 || d.wearPct != UINT32_MAX || d.sparePct != UINT32_MAX)) {
            *err = L"NVMe 健康细节字段在未读到日志时被填充（违反诚实原则）";
            return false;
        }
    }

    // --- 运行时长：真实单调值，绝不为 0/负。
    if (!(snap.uptimeSec > 0.0)) {
        *err = L"uptimeSec 非法（应 >0）";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// lhm_off_by_default：桥接出厂禁用，用户打开开关前绝不触网。
//
// ---------------------------------------------------------------------------
STM_TEST(lhm_off_by_default) {
    // 显式重置为默认，使本测试与顺序无关。
    stm::SetLhmOptions(stm::LhmOptions{});
    const stm::LhmOptions o = stm::GetLhmOptions();
    if (o.enabled || o.port != 8085 || o.host != L"127.0.0.1") {
        *err = L"LHM 默认选项不是 disabled/8085/127.0.0.1";
        return false;
    }
    std::vector<stm::SensorReading> out;
    std::wstring err2;
    if (stm::PollLhm(&out, &err2)) {
        *err = L"未启用时 PollLhm 不应成功";
        return false;
    }
    if (err2.empty()) {
        *err = L"未启用时 PollLhm 未给出错误说明";
        return false;
    }
    if (!out.empty()) {
        *err = L"未启用时 PollLhm 却返回了读数";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// lhm_parse_minijson：内置迷你 JSON 解析器把内嵌的样例
// data.json（嵌套 + 转义 + 单位 + null 值）映射为 SensorReading。
// ---------------------------------------------------------------------------
STM_TEST(lhm_parse_minijson) {
    // 镜像 LibreHardwareMonitor 远程 web 服务器模式：嵌套的
    // Hardware/Children 节点带 "Sensor" 数组；一个 Value 带
    // \uXXXX 单位后缀，一个 Text 带转义，一个 Value 为 null。
    static const char kSample[] =
        "{\"id\":0,\"Text\":\"LibreHardwareMonitor\",\"Value\":null,\"Children\":["
        "{\"id\":1,\"Text\":\"DESKTOP-F3\",\"Value\":null,\"Children\":["
        "{\"id\":2,\"Text\":\"Intel Core i7-10700\",\"Value\":null,\"Children\":["
        "{\"id\":3,\"Text\":\"Temperatures\",\"Value\":null,\"Children\":[],"
        "\"Sensor\":[{\"Text\":\"Core Average\",\"Value\":\"45.000\"},"
        "{\"Text\":\"Core \\\"boost\\\" #1\",\"Value\":\"52.000\"}]}"
        "],\"Sensor\":[]},"
        "{\"id\":4,\"Text\":\"nvme SSD\",\"Value\":null,\"Children\":[],"
        "\"Sensor\":[{\"Text\":\"Drive Temperature\",\"Value\":\"38.000\\u00b0C\"},"
        "{\"Text\":\"Broken\",\"Value\":null}]}"
        "],\"Sensor\":[]}"
        "],\"Sensor\":[]}";

    std::vector<stm::SensorReading> out;
    if (!stm::ParseLhmJson(kSample, &out)) {
        *err = L"样例 data.json 解析失败";
        return false;
    }
    if (out.size() < 3) {
        *err = stm::Fmt(L"解析出的读数过少：{}", out.size());
        return false;
    }

    const stm::SensorReading* coreAvg = nullptr;
    const stm::SensorReading* escaped = nullptr;
    const stm::SensorReading* nvmeTemp = nullptr;
    const stm::SensorReading* broken = nullptr;
    for (const stm::SensorReading& r : out) {
        if (r.label.find(L"Core Average") != std::wstring::npos) coreAvg = &r;
        if (r.label.find(L"boost") != std::wstring::npos) escaped = &r;
        if (r.label.find(L"Drive Temperature") != std::wstring::npos) nvmeTemp = &r;
        if (r.label.find(L"Broken") != std::wstring::npos) broken = &r;
        if (r.label.find(L"［LHM］") == std::wstring::npos) {
            *err = L"读数缺少 LHM 来源标注";
            return false;
        }
    }
    if (coreAvg == nullptr || coreAvg->value != 45.0 || coreAvg->unit != L"" ||
        coreAvg->state != State::Ok ||
        coreAvg->label.find(L"Intel Core i7-10700 / Temperatures") == std::wstring::npos) {
        *err = L"Core Average 映射错误（label/value/unit）";
        return false;
    }
    if (escaped == nullptr || escaped->value != 52.0 ||
        escaped->label.find(L"Core \"boost\" #1") == std::wstring::npos) {
        *err = L"转义字符串处理错误";
        return false;
    }
    if (nvmeTemp == nullptr || nvmeTemp->value != 38.0 || nvmeTemp->unit != L"°C" ||
        nvmeTemp->state != State::Ok ||
        nvmeTemp->label.find(L"nvme SSD") == std::wstring::npos) {
        *err = L"NVMe 温度映射错误（\\u00b0C 应解码为 °C）";
        return false;
    }
    if (broken == nullptr || broken->state != State::NoHardware) {
        *err = L"Value=null 应映射为 NoHardware（而不是 0）";
        return false;
    }

    // 畸形输入必须干净失败且无读数。
    const wchar_t* junkCases[] = {L"not json", L"{\"Children\":", L"[1,2,", L"{}extra"};
    for (const wchar_t* why : junkCases) {
        std::vector<stm::SensorReading> junk;
        const std::string narrow = stm::WideToUtf8(why);
        if (stm::ParseLhmJson(narrow, &junk)) {
            *err = stm::Fmt(L"非法输入被误接受：{}", why);
            return false;
        }
        if (!junk.empty()) {
            *err = L"解析失败时输出未清空";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// lhm_localhost_only：红线——客户端拒绝非回环主机，
// 然后恢复默认，让其他测试见到初始状态。
// ---------------------------------------------------------------------------
STM_TEST(lhm_localhost_only) {
    stm::LhmOptions o;
    o.enabled = true;
    o.host = L"192.168.1.10";
    stm::SetLhmOptions(o);

    std::vector<stm::SensorReading> out;
    std::wstring err2;
    if (stm::PollLhm(&out, &err2)) {
        *err = L"非本机地址被允许（违反仅 localhost 红线）";
        stm::SetLhmOptions(stm::LhmOptions{});
        return false;
    }
    if (err2.find(L"本机") == std::wstring::npos) {
        *err = L"拒绝非本机地址时未说明原因";
        stm::SetLhmOptions(stm::LhmOptions{});
        return false;
    }
    if (!out.empty()) {
        *err = L"被拒绝的轮询却返回了读数";
        stm::SetLhmOptions(stm::LhmOptions{});
        return false;
    }
    stm::SetLhmOptions(stm::LhmOptions{});
    return true;
}
