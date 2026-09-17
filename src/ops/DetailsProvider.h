#pragma once
// On-demand per-process details, fetched via the ops JobQueue and cached (arch section 8).
// UI-thread-only API surface (Request/Peek); worker fills cache and posts a notification.
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/Jobs.h"
#include "core/Notifications.h"
#include "core/ProcData.h"
#include "ops/Signature.h"

namespace stm {
namespace ops {

enum class DetailKind : uint32_t {
    Signature = 1u << 0,  // WinVerifyTrust result
    CmdLine   = 1u << 1,  // command line (needs VM_READ; empty + cmdLineAvail=false without rights)
    UserInfo  = 1u << 2,  // owning user name
    GuiObjects= 1u << 3,  // GDI/USER object counts
    Modules   = 1u << 4,  // module list (elevated view may differ; honest on failure)
};

struct ProcessDetails {
    SigState sig = SigState::Unknown;
    bool sigResolved = false;
    std::wstring cmdLine;
    bool cmdLineAvail = false;
    std::wstring userName;
    bool userNameResolved = false;
    uint32_t gdiObjects = 0, userObjects = 0;
    bool guiResolved = false;
    std::vector<std::wstring> modules;
    bool modulesResolved = false;
};

class DetailsProvider {
public:
    DetailsProvider(JobQueue& jobs, NotificationQueue& notes) : jobs_(jobs), notes_(notes) {}

    // Submit fetch for the given kinds if not already pending. path is the snapshot's image path.
    void Request(const ProcKey& key, const std::wstring& path, uint32_t kinds);
    // UI thread only. Returns nullptr when nothing was fetched yet.
    const ProcessDetails* Peek(const ProcKey& key) const;
    void Invalidate(const ProcKey& key);
    // Merge GuiObjects into ProcInfo for table display (UI thread).
    bool TryGetGuiObjects(const ProcKey& key, uint32_t* gdi, uint32_t* user) const;

private:
    struct Entry {
        bool pending = false;
        ProcessDetails data;
    };
    Entry* BeginEntry(const ProcKey& key);  // locked
    JobQueue& jobs_;
    NotificationQueue& notes_;
    mutable std::mutex mu_;
    std::unordered_map<ProcKey, Entry> cache_;
};

}  // namespace ops
}  // namespace stm
