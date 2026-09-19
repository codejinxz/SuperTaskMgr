#pragma once
// WinVerifyTrust 包装（有文档 API；VERIFY 必须与 STATEACTION_CLOSE 配对）。
// 系统文件是目录签名的 => 包含目录验证路径。
// 必须在 ops 工作线程运行（毫秒级；吊销检查可能触盘/触网——
// 吊销沿用系统默认；绝不阻塞 UI 线程）。
#include <string>

namespace stm {
namespace ops {

enum class SigState { Unknown, Valid, Unsigned, Invalid, NoCheck };

// path：完整映像路径。空路径 / 文件缺失时返回 NoCheck。
SigState VerifyFileSignature(const std::wstring& path);

}  // namespace ops
}  // namespace stm
