#pragma once
namespace stm {

class Theme {
public:
    // Dark Windows-11-ish palette; call once after ImGui init (before first frame).
    static void Apply();
};

}  // namespace stm
