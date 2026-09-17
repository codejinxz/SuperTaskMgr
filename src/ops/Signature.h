#pragma once
// WinVerifyTrust wrapper (documented API; VERIFY must pair with STATEACTION_CLOSE).
// System files are catalog-signed => catalog verification path included.
// Must run on the ops worker thread (ms-level, may touch disk/network for revocation —
// revocation left to system defaults; we never block the UI thread).
#include <string>

namespace stm {
namespace ops {

enum class SigState { Unknown, Valid, Unsigned, Invalid, NoCheck };

// path: full image path. Returns NoCheck for empty path / missing file.
SigState VerifyFileSignature(const std::wstring& path);

}  // namespace ops
}  // namespace stm
