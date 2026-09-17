#pragma once
// Built-in critical process protection list (arch section 6; contract moved from ops to core
// so that stm_collect can flag PF_Protected without depending on stm_ops).
#include <cstdint>
#include <string>

namespace stm {

// Returns a non-empty Chinese reason string if (pid,name,path) is protected, else empty.
// Matching: pid 0/4, or exact case-insensitive image name against the built-in list.
std::wstring ProtectedReason(uint32_t pid, const std::wstring& name, const std::wstring& path);

// Convenience: sets PF_Protected in *flags when protected.
void MarkProtectedFlag(uint32_t pid, const std::wstring& name, const std::wstring& path, uint32_t* flags);

}  // namespace stm
