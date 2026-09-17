#pragma once
// Page registry + app shell chrome (toolbar / tab bar / status bar).
// NOTE: Pages.cpp is owned by the UI developer agent from phase 2 onward;
// the shell only guarantees RegisterPages() + DrawShell() signatures.
namespace stm {

class AppContext;

void RegisterPages(AppContext& ctx);   // fill ctx.pages (tab order)
void DrawShell(AppContext& ctx);       // toolbar + tabs + status bar + active page

}  // namespace stm
