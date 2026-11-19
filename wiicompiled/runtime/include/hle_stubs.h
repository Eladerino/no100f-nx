#pragma once

#include "abi_bridge.h"

// hle/gx/gx_fatal_stubs.cpp includes nothing but this header and reaches
// std::fprintf / std::snprintf / std::abort through it.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// VI Utils
void VI_HLE_ForceRetrace(CpuContext* ctx);
void VI_HLE_PollRetrace(CpuContext* ctx);
void VI_HLE_ProcessRetracesDeferred(int maxToProcess);
// Stall-service stage A (hle/vi.cpp): advance the retrace count and its guest
// mirror under g_viMutex, delivering nothing - no queue wake, no callbacks, no
// guest code. The counter-only twin of VI_HLE_ProcessRetracesDeferred.
void VI_HLE_AdvanceRetraceCountOnly();
void VI_HLE_WaitForNextRetracePoll();
// Deterministic mode (det_clock.h): the virtual tick of the next VI retrace, DetClock::kNever
// before VIInit.
uint64_t VI_HLE_DetNextRetraceTicks();
// Single owner of the Aurora frame presentation sequence (seal, optional pace
// to the VI retrace boundary, pre-warm the next frame). paceToRetrace is true
// for the GXCopyDisp producer path and false for retrace-context presents.
void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace);
bool VI_HLE_IsAdvancingRetrace();
void VI_HLE_SetXfbReady(uint32_t xfbAddr); // Called by GXCopyDisp to signal EFB→XFB copy
void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros);
void Audio_HLE_Poll(CpuContext* ctx);
// Deferred twin of Audio_HLE_Poll for the long host waits that already service
// retraces and alarms (the VI retrace pacing loop, the Aurora frame-worker wait
// callback). Runs the AI DMA tick on an isolated register file the way
// OS_HLE_ProcessAlarmsDeferred does, so it is safe to call from the middle of an
// arbitrary translated function.
void Audio_HLE_PollDeferred();
// Deterministic mode: the virtual tick at which the AI DMA block in flight completes, or
// DetClock::kNever while audio DMA is stopped.
uint64_t Audio_HLE_DetNextEventTicks();
// tracecnt sampler (hle/vi.cpp, SCOOBY_TRACE_CNT): host-side reads of the AI DMA delivery
// counters the tick paths in hle/audio/audio.cpp maintain. Diagnostics only.
uint32_t Audio_HLE_TraceAiCallbacks();
uint32_t Audio_HLE_TraceAiBlocks();
// tracecnt sampler: the OS alarm queue head, its fire time and the current system time
// (all system-time ticks), read host-side in hle/os/os_alarm.cpp beside NextAlarmTicks.
// False when the queue is empty or unreadable; never fires or unlinks anything.
bool OS_HLE_TraceAlarmHead(uint32_t* head, uint64_t* fireTime, uint64_t* now);
// stall-service feeder (hle/vi.cpp): host-side read of the async-completion enqueue
// counter hle/storage/dvd.cpp maintains at QueueDvdCallback, the choke point every
// DVD/CARD completion queues through. Gating only; no state is touched.
uint32_t Dvd_HLE_TraceEnqueues();
// Follow-up instrument (SEAMFIX-FOLLOWUP-STUDY, Nov 16, 2026): per-boot counters
// for the two GX completion sites (hle/gx/gx_fifo.cpp) and the held GX deque's
// high-water mark (hle/storage/dvd.cpp), read by the tracecnt sampler in
// hle/vi.cpp ([gxcnt] lines). Diagnostics only; the counters are maintained
// unconditionally and touch no guest-visible state.
uint32_t GxFifo_HLE_BreakPtCalls();
uint32_t GxFifo_HLE_PeHitsToken();
uint32_t GxFifo_HLE_PeHitsDrawDone();
uint32_t GxFifo_HLE_PeHitsSync();
uint32_t GxFifo_HLE_PeHitsOther();
uint32_t Dvd_HLE_GxHeldHighWater();
// Drain-at-retrace (RETRACE-HALF-STUDY R5): the held GX deque's current depth
// and the boot-total of callbacks DvdProcessPendingGxCallbacks has delivered,
// read by the retrace drain in hle/vi.cpp for its gate and its drains/drained
// counters. Diagnostics only; no state is touched.
uint32_t Dvd_HLE_GxPendingCount();
uint32_t Dvd_HLE_GxDeliveredTotal();
// Fix 2 (FIX2-DESIGN-STUDY Part B): per-present completion flag (hle/gx/gx_fifo.cpp).
// NotePresentCompletion is called at the two gx_fifo queue sites that queue a
// frame-swap completion; TakePresentCompletion is called only by the Fix 2
// present hook in hle/vi.cpp, which reads and clears the flag once per present
// as the input to its synthesis decision. Host-side bookkeeping only.
void GxFifo_HLE_NotePresentCompletion();
bool GxFifo_HLE_TakePresentCompletion();
bool OS_HLE_InterruptsEnabled() noexcept;
// tracewake bookkeeping (hle/os/os_interrupt.cpp): true while this host thread is inside
// DeliverPendingInterruptsOnEnable, the restore-time delivery entry. The OSWakeupThread
// seam logger in hle/os/os_scheduler.cpp reads it to flag wakes executed inside a
// restore-time delivery. Maintained only when SCOOBY_TRACE_WAKE is set; diagnostics only.
bool OS_HLE_InRestoreDelivery();
// tracewake (park seam trace, Nov 16, 2026; PARK-STUDY.md Q4(d)): shared seam state
// for the [wake] loggers in hle/os/os_sleep.cpp and hle/os/os_scheduler.cpp.
// kTraceWakeQueueAddr is the display-sync object's OSThreadQueue; the snapshot words
// are the sync counters around it (object map: PARK-STUDY.md Q1). The two functions
// are defined in hle/os/os_scheduler.cpp; diagnostics only, inert unless
// SCOOBY_TRACE_WAKE is set.
inline constexpr uint32_t kTraceWakeQueueAddr = 0x802716E4u;
struct TraceWakeSync {
    uint32_t counterA = 0;  // 0x802716C0: displayed-buffer index; the advancer writes (A+1) mod 3
    uint32_t counterB = 0;  // 0x802716C4: comparison index the waiter diffs against counterA
    uint32_t arm = 0;       // 0x802716D8: "frame drawn" arm flag
    uint32_t latch = 0;     // 0x802716DC: per-retrace latch
    uint32_t guest = 0;     // 0x80271398: VI retrace count word (kViRetraceCountAddr in hle/vi.cpp)
    uint32_t d4 = 0;        // 0x802716D4: AF0's flip-charge word (RETRACE-HALF-STUDY R1)
    uint32_t b8 = 0;        // 0x802716B8: display-sync mode state flag (advancer charge cadence)
    uint32_t e0 = 0;        // 0x802716E0: advancer bookkeeping toggle (charges alternate runs when B8 != 0)
    uint32_t flag = 0;      // 0x802716D0: presenter site-2 gate flag - set by the presenter's site-2 path,
                            // cleared only by the advancer's catch-up branch (CLIFF-STUDY); added by
                            // DRAIN-DURABILITY-STUDY Q4 so the phase-slip state reads directly
};
bool OS_HLE_TraceWakeArmed();
TraceWakeSync OS_HLE_TraceWakeReadSync();
// parkwatch (park rescue, Nov 16, 2026; PARK-STUDY-2.md Q4(c)): the detector in
// hle/vi.cpp calls this when the display-sync park signature confirms;
// SelectThread's idle loop in hle/os/os_scheduler.cpp consumes the request and
// fires one OS_HLE_WakeupThreadNoReschedule on the display-sync queue, on the
// scheduler thread. Defined in hle/os/os_scheduler.cpp; the request is only
// ever set when SCOOBY_PARKWATCH is set.
void OS_HLE_RequestParkWake();

// seamfix (SEAMFIX-DESIGN.md): deliver-after-link at the OSSleepThread seam plus
// deferral of the GX PE-interrupt callback class (the QueueGuestCallback sites
// inside RaisePeInterruptForBp, hle/gx/gx_fifo.cpp) to interrupt-shaped delivery
// points only - the seam pass in hle/os/os_sleep.cpp and the scheduler idle
// pump - instead of the eager general drain. Armed by dk_diag seamfix
// (SCOOBY_SEAMFIX); unarmed, GX callbacks take the general drain exactly as
// before and the seam pass does not exist. OS_HLE_SeamFixArmed is defined in
// hle/os/os_scheduler.cpp beside OS_HLE_TraceWakeArmed;
// DvdProcessPendingGxCallbacks is the held queue's drain in
// hle/storage/dvd.cpp.
bool OS_HLE_SeamFixArmed();
bool DvdProcessPendingGxCallbacks(CpuContext* cpu);
// The held GX deque's enqueue (hle/storage/dvd.cpp), declared here for the
// Fix 2 present hook in hle/vi.cpp (hle/gx/gx_fifo.cpp declares it locally).
void QueueGxInterruptCallback(uint32_t callbackPtr, uint32_t r3, uint32_t r4);
// Fix 2 (FIX2-DESIGN-STUDY Part B): present-time completion synthesis. Armed
// by dk_diag fix2 (SCOOBY_FIX2), a separate key from seamfix so the seamfix
// A/B stays uncontaminated; the arm check is defined in
// hle/os/os_scheduler.cpp beside OS_HLE_SeamFixArmed. Unarmed, the present
// hook in hle/vi.cpp evaluates the arm flag and returns - behaviour
// identical to the seam-wake build.
bool OS_HLE_Fix2Armed();
// seam-wake (CLIFF-STUDY.md Q4 rank 1): parkwatch's actuation helper, defined
// in hle/os/os_scheduler.cpp beside WakeupThreadQueue - one wake on a wait
// queue, no reschedule. The seam pass in hle/os/os_sleep.cpp calls it directly
// when the display-sync sleeper is still linked at pipeline-full after the
// delivery steps (see the seam-wake block there).
void OS_HLE_WakeupThreadNoReschedule(CpuContext* ctx, uint32_t waitQueue);
// durability (DRAIN-DURABILITY-STUDY Q4): read-only total of the seam-wake
// fires counted in hle/os/os_sleep.cpp (the [seamwake] fire counter), read by
// the Fix 2 present hook's health line in hle/vi.cpp. Diagnostics only; the
// counter is incremented exactly where it always was - only its storage
// moved to file scope so this accessor can read it.
uint32_t OS_HLE_SeamWakeFires();
extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess);
extern "C" void OS_HLE_BeginDeferredGuestCallbacks();
extern "C" void OS_HLE_EndDeferredGuestCallbacks();


// Defines and registers a faithful native reimplementation that REPLACES the translated function
// at a PPC address (not a stub; genuine not-yet-implemented entries live in hle/gx/gx_fatal_stubs.cpp
// and abort). The translator regex-parses these macro names to skip that address at build time, so
// renaming requires updating Translator.Cli/Program.cs, RuntimeNativeGuestEffectAnalyzer.cs,
// TranslatedBuildShardEmitter.cs and RuntimeNativeFunctionAbiProvider.cs together.
//
// An override removes the game's function, so it has to port all of it. To change only a function's inputs or when
// it runs, wrap it instead: REGISTER_NATIVE_FUNCTION_AS(0x<addr>, name, "name") takes the address over but keeps the
// game's function translated as func_<addr>, which the wrapper calls (callers reach the wrapper by address). With
// its option off, a wrapper should be a plain call of func_<addr>, so the game runs its own code. Example:
// hle/no100f_particles.cpp. Compare a wrapper with main using tools/det-check.py (docs/testing.md, 4d).

#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list) \
    extern "C" ret_type func_##addr_hex arg_list { return name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)

#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list) \
    extern "C" void func_##addr_hex arg_list { name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)
