#pragma once
// Game-thread CPU log and core placement (lane AJ), Linux only; no-ops elsewhere.
//
// KARTFRAME_CPULOG=1: one `cpulog:` line a second from a low-cost monitor thread: game frames, the game
// thread's CPU, run-queue wait, pacing sleep and our hooks per frame, the cores it ran on, the busiest
// threads of the process (on-CPU and run-queue ms a second, last core), each core's clock and the
// hottest thermal zone. The game thread only bumps counters (a clock read around each timed part).
// KARTFRAME_GAME_CORE=<n>|auto[:shared]: pins the game thread to core n (auto: the allowed core with the
// largest cpu_capacity) and, unless :shared, keeps every other thread of the process off it (re-applied
// every second, so threads started later are moved too).
#include <cstdint>

namespace cpumon {

enum Slot : uint32_t {
    kPace = 0,     // the game thread sleeping to the VI retrace boundary
    kVrCommit,     // first-person commit + anchor + held item at the present
    kVrUpdate,     // MkwVRFirstPersonUpdate (race draw hook)
    kUiHook,       // layout control hook (nameplate depth)
    kOverlay,      // settings overlay draw + ImGui frame end/begin
    kEvents,       // aurora update / SDL event pump at the present
    kGxSync,       // KARTFRAME_GX_SYNC=present: waiting for the GX thread at the present
    kSlotCount
};

// True when anything is being measured (KARTFRAME_CPULOG=1); read once.
bool Timing() noexcept;
void Add(Slot slot, uint64_t ns) noexcept;
uint64_t NowNs() noexcept;
// Once per game frame, on the game thread (VI present). Starts the monitor on the first call.
void GameFrame() noexcept;

struct Scope {
    Slot slot;
    uint64_t start;
    explicit Scope(Slot s) noexcept : slot(s), start(Timing() ? NowNs() : 0) {}
    ~Scope() {
        if (start != 0) Add(slot, NowNs() - start);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace cpumon
