#pragma once
// Page extension contract (arch section 8): every tab implements IPage and registers
// itself in the app shell's page list. Phase 3 pages plug in without touching the shell.
#include <string>

namespace stm {

class AppContext;  // app/AppContext.h (circular-break: pages include it, it includes this)

class IPage {
public:
    virtual ~IPage() = default;
    virtual const wchar_t* Id() const = 0;     // stable id for config persistence
    virtual const wchar_t* Title() const = 0;  // display title (Chinese)
    virtual void Draw(AppContext& ctx) = 0;    // called once per frame when tab is active
};

}  // namespace stm
