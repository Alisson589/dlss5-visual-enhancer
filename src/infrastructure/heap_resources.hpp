#pragma once
#include <windows.h>

namespace ve {
// Return unused Windows LFH pages after the job's owners have closed. Live
// allocations and retained native runtimes remain owned normally. This is a
// best-effort OS hint, so failure must never turn a completed job into an error.
inline void reclaimIdleHeapCaches() noexcept {
    HEAP_OPTIMIZE_RESOURCES_INFORMATION options{HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION,0};
    (void)HeapSetInformation(nullptr,HeapOptimizeResources,&options,sizeof(options));
}
} // namespace ve
