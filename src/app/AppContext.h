#pragma once
// Application context handed to every IPage (arch section 8). Contract header — architect-owned.
// Lifetime: constructed in main before the message loop; services started before pages draw.
#include <memory>
#include <vector>
#include "core/Cfg.h"
#include "core/Jobs.h"
#include "core/Notifications.h"
#include "collect/CollectService.h"
#include "ops/DetailsProvider.h"
#include "ui/IPage.h"

namespace stm {

class AppContext {
public:
    Config cfg;
    CollectService collect;
    JobQueue jobs;                    // ops worker queue
    NotificationQueue notes;          // worker -> UI results
    std::unique_ptr<ops::DetailsProvider> details;

    bool elevated = false;
    bool wantExit = false;            // set by menu/confirm dialogs
    // Status bar self-observability (UI thread updates each frame).
    double frameMs = 0;

    // Page registry (app shell owns instances; order = tab order).
    std::vector<std::unique_ptr<IPage>> pages;
    int activePage = 0;
};

}  // namespace stm
