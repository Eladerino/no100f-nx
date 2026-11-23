// OSSleepTicks / OSSleepThread parking plus the host-side sleep-timer table.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "det_clock.h"
#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "fiber_manager.h"
#include "timebase_contract.h"
#include "runtime_log.h"
#include "os_internal.h"

#include "../no100f_native_scene.h"

namespace {
// Sleep deadlines live on the virtual timeline in deterministic mode (see det_clock.h), so a
// sleeper wakes at the same point of guest execution on every device.
//
// N8 C7: SleepClockNow clock reads (steady_clock branch only, not DetClock).
// Gated on the nativescene dk_diag key (No100fNativeSceneEnabled), one branch
// per site when off. Counters ride the measurement flight only, never a price build.
static std::atomic<uint64_t> s_n8ClockOsSleep{0};
std::chrono::steady_clock::time_point SleepClockNow()
{
    if (DetClock::Enabled()) return DetClock::NowTimePoint();
    if (No100fNativeSceneEnabled()) s_n8ClockOsSleep.fetch_add(1, std::memory_order_relaxed);
    return std::chrono::steady_clock::now();
}
} // namespace

// N8 C7: SleepClockNow clock reads since start (the runtime's [n8counters] line).
uint64_t ossleep_n8_clock_total() {
  return s_n8ClockOsSleep.load(std::memory_order_relaxed);
}

namespace OsHleInternal {
std::mutex gSleepTimerMutex;
std::vector<SleepTimerEntry> gSleepTimers;

void CancelSleepTimer(uint32_t threadPtr)
{
    std::lock_guard<std::mutex> lock(gSleepTimerMutex);
    std::erase_if(gSleepTimers, [threadPtr](const SleepTimerEntry& entry) {
        return entry.threadPtr == threadPtr;
    });
}

uint64_t NextSleepTimerTicks()
{
    std::lock_guard<std::mutex> lock(gSleepTimerMutex);
    uint64_t next = DetClock::kNever;
    for (const SleepTimerEntry& entry : gSleepTimers) {
        next = std::min(next, DetClock::TicksAtOrAfter(entry.deadline));
    }
    return next;
}

bool SleepTimerIsPending(uint32_t threadPtr)
{
    std::lock_guard<std::mutex> lock(gSleepTimerMutex);
    for (const SleepTimerEntry& entry : gSleepTimers) {
        if (entry.threadPtr == threadPtr) {
            return true;
        }
    }
    return false;
}

// Threads with an outstanding OSSleepTicks park (timer armed, wake not yet delivered). If one ends
// up park-shaped with no pending timer, the reconciler in ProcessSleepTimers heals it; nothing
// outside this set is ever force-started.
std::mutex gOutstandingParkMutex;
std::unordered_set<uint32_t> gOutstandingParks;

void MarkParkOutstanding(uint32_t threadPtr)
{
    std::lock_guard<std::mutex> lock(gOutstandingParkMutex);
    gOutstandingParks.insert(threadPtr);
}

void ClearOutstandingPark(uint32_t threadPtr)
{
    std::lock_guard<std::mutex> lock(gOutstandingParkMutex);
    gOutstandingParks.erase(threadPtr);
}

void ScheduleSleepTimer(uint32_t threadPtr, uint64_t ticks)
{
    using Clock = std::chrono::steady_clock;

    const auto duration = TimeBaseContract::TicksToDuration(ticks);
    const auto deadline = SleepClockNow() + duration;

    std::lock_guard<std::mutex> lock(gSleepTimerMutex);
    std::erase_if(gSleepTimers, [threadPtr](const SleepTimerEntry& entry) {
        return entry.threadPtr == threadPtr;
    });
    gSleepTimers.push_back({threadPtr, deadline});
}

bool ProcessSleepTimers(CpuContext* cpu)
{
    using Clock = std::chrono::steady_clock;

    // Pop and process ONE due timer at a time, straight from the shared table. Resuming a
    // sleeper re-enters the scheduler (OSResumeThread -> SelectThread) and can switch fibers
    // away from this call. Timers that had already been popped into a private list would then
    // sit on the suspended fiber's stack with their threads parked and no entry in the table:
    // exactly the "park-shaped with no pending wake timer" strand the reconciler below heals
    // 100ms late, followed by a "sleep-timer stale" drop when this fiber finally resumes.
    // Leaving unprocessed timers in the table keeps them visible to every other pump (idle
    // loop, other threads' SelectThread) while this one is switched away.
    bool processedAny = false;
    constexpr size_t kMaxTimersPerCall = 64;
    size_t processedCount = 0;
    const auto now = SleepClockNow();
    while (processedCount < kMaxTimersPerCall) {
        SleepTimerEntry timer{0, {}};
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(gSleepTimerMutex);
            for (auto it = gSleepTimers.begin(); it != gSleepTimers.end(); ++it) {
                if (it->deadline <= now) {
                    timer = *it;
                    gSleepTimers.erase(it);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            break;
        }
        ++processedCount;
        processedAny = true;

        const uint32_t threadPtr = timer.threadPtr;
        if (threadPtr == 0 ||
            !Memory::Contains(threadPtr + kThreadSuspendOffset, sizeof(uint32_t))) {
            continue;
        }

        // A sleep park always leaves the thread Ready with a suspension. A Running thread with a due
        // timer is mid-park (fire raced the self-suspend), so defer instead of consuming it there or
        // the sleeper strands. Any other shape means the sleeper already woke; drop the stale timer
        // without touching the thread.
        const uint16_t stateAtFire = Memory::Read16(threadPtr + kThreadStateOffset);
        const int32_t countAtFire =
            static_cast<int32_t>(Memory::Read32(threadPtr + kThreadSuspendOffset));
        if (stateAtFire == kThreadStateRunning) {
            std::lock_guard<std::mutex> lock(gSleepTimerMutex);
            bool present = false;
            for (const SleepTimerEntry& entry : gSleepTimers) {
                if (entry.threadPtr == threadPtr) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                gSleepTimers.push_back({threadPtr, now + std::chrono::milliseconds(2)});
            }
            continue;
        }
        if (stateAtFire != kThreadStateReady || countAtFire < 1) {
            RT_LOG(RT_TAG_OS) << "sleep-timer stale for thread 0x" << std::hex << threadPtr
                      << std::dec << " (state=" << stateAtFire << " suspends=" << countAtFire
                      << "); dropped without consuming a suspension" << std::endl;
            ClearOutstandingPark(threadPtr);
            continue;
        }

        ClearOutstandingPark(threadPtr);
        cpu->gpr[3] = threadPtr;
        OSResumeThread_HLE_801aa58c(cpu);

        // OSResumeThread only peels one suspension level, so if the sleeper's count is inflated
        // (failed park, overlapping suspend) dropping the timer here would strand it. Keep it armed
        // instead of consuming it; remaining suspensions after the park's own are foreign ones woken
        // by their own OSResumeThread. Rearming here used to steal those foreign suspensions, which
        // is how the DWC re-init thread got stranded.
    }

    // Stranded-sleeper reconciler: heals a thread whose park has no pending wake timer (lost to a
    // race) by resuming it once that shape persists for 100ms, sampled every 50ms so no strand is
    // missed. The atomic CAS below lets exactly one caller run the scan when ProcessSleepTimers
    // executes on more than one host thread; the rest skip it.
    static std::atomic<Clock::rep> strandScanDueAt{0};
    auto strandScanClaim = strandScanDueAt.load(std::memory_order_relaxed);
    const bool runStrandReconciler =
        now.time_since_epoch().count() >= strandScanClaim &&
        strandScanDueAt.compare_exchange_strong(
            strandScanClaim,
            static_cast<Clock::rep>(
                (now + std::chrono::milliseconds(50)).time_since_epoch().count()),
            std::memory_order_relaxed);
    if (runStrandReconciler) {
        static std::mutex strandMutex;
        static std::unordered_map<uint32_t, Clock::time_point> strandFirstSeen;

        std::vector<uint32_t> outstanding;
        {
            std::lock_guard<std::mutex> lock(gOutstandingParkMutex);
            outstanding.assign(gOutstandingParks.begin(), gOutstandingParks.end());
        }
        // Decide who to heal under the lock, but resume OUTSIDE it: the resume
        // re-enters the scheduler and thus this function, and holding the lock
        // across it is a guaranteed self-deadlock.
        std::vector<uint32_t> toHeal;
        {
            std::lock_guard<std::mutex> strandLock(strandMutex);
            // Drop debounce records for threads whose park was satisfied elsewhere.
            for (auto it = strandFirstSeen.begin(); it != strandFirstSeen.end();) {
                if (std::find(outstanding.begin(), outstanding.end(), it->first) ==
                    outstanding.end()) {
                    it = strandFirstSeen.erase(it);
                } else {
                    ++it;
                }
            }
            const auto reconcileNow = SleepClockNow();
            for (const uint32_t threadPtr : outstanding) {
                if (SleepTimerIsPending(threadPtr)) {
                    strandFirstSeen.erase(threadPtr);
                    continue;
                }
                if (!Memory::Contains(threadPtr + kThreadSuspendOffset, sizeof(uint32_t)) ||
                    Fiber::GuestFiberManager::IsTerminated(threadPtr)) {
                    ClearOutstandingPark(threadPtr);
                    strandFirstSeen.erase(threadPtr);
                    continue;
                }
                const uint16_t state = Memory::Read16(threadPtr + kThreadStateOffset);
                const int32_t count =
                    static_cast<int32_t>(Memory::Read32(threadPtr + kThreadSuspendOffset));
                if (state != kThreadStateReady || count < 1) {
                    ClearOutstandingPark(threadPtr);
                    strandFirstSeen.erase(threadPtr);
                    continue;
                }
                const auto seen = strandFirstSeen.find(threadPtr);
                if (seen == strandFirstSeen.end()) {
                    strandFirstSeen.emplace(threadPtr, reconcileNow);
                    continue;
                }
                if (reconcileNow - seen->second < std::chrono::milliseconds(100)) {
                    continue;
                }
                strandFirstSeen.erase(seen);
                ClearOutstandingPark(threadPtr);
                toHeal.push_back(threadPtr);
            }
        }
        for (const uint32_t threadPtr : toHeal) {
            RT_LOG(RT_TAG_OS) << "reconciler: thread 0x" << std::hex << threadPtr
                      << std::dec << " park-shaped with no pending wake timer for 100ms; "
                      << "resuming lost sleeper" << std::endl;
            cpu->gpr[3] = threadPtr;
            OSResumeThread_HLE_801aa58c(cpu);
        }
    }

    return processedAny;
}
} // namespace OsHleInternal

// ---------------------------------------------------------------------------
// OSSleepTicks HLE - mirror the SDK behavior by suspending the current guest
// thread and resuming it from a host-side timer. A host sleep here starves
// lower-priority guest workers because the scheduler never gets control.
// ---------------------------------------------------------------------------
extern "C" void OS__SleepTicks_HLE_801aaca8(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) {
        return;
    }

    const uint64_t ticks = (static_cast<uint64_t>(cpu->gpr[3]) << 32) | cpu->gpr[4];
    const int32_t irqState = OS__DisableInterrupts_801a65ac();

    try {
        uint32_t currentThread = ::Memory::Read32(kOSRunningContextAddr);
        if (Fiber::GuestFiberManager::IsInitialized()) {
            const uint32_t fiberThread = Fiber::GuestFiberManager::GetCurrentGuestThread();
            if (fiberThread != 0) {
                currentThread = fiberThread;
            }
        }
        if (currentThread == 0) {
            OS__RestoreInterrupts_801a65d4(irqState);
            return;
        }

        // Parking here is OSSuspendThread plus a one-shot timer that can only peel one suspension.
        // If the scheduler cannot switch away (OSDisableScheduler nesting raised around a deferred
        // guest-callback batch), suspending would inflate the count with nothing left to zero it,
        // parking forever (the silent DWC SOStartupEx hang). Mirror OSSleepThread: refuse to park
        // and let the caller retry.
        const uint32_t sleepIdleFlag = ::Memory::Read32(kSchedulerIdleFlagAddr);
        const uint32_t sleepCurrentContext = ::Memory::Read32(kOSCurrentContextAddr);
        if (sleepIdleFlag != 0 ||
            (sleepCurrentContext != 0 && sleepCurrentContext != currentThread)) {
            thread_local std::unordered_set<uint32_t> reportedUnparkableTickSleeps;
            if (reportedUnparkableTickSleeps.insert(currentThread).second) {
                RT_LOG(RT_TAG_OS) << "OSSleepTicks: thread 0x" << std::hex << currentThread
                          << std::dec << " cannot park (scheduler nesting "
                          << sleepIdleFlag << "); busy-returning so the caller retries."
                          << std::endl;
            }
            OS__RestoreInterrupts_801a65d4(irqState);
            return;
        }

        ScheduleSleepTimer(currentThread, ticks);
        MarkParkOutstanding(currentThread);
        cpu->gpr[3] = currentThread;
        OSSuspendThread_HLE_801aa6a8(cpu);

        // Defence in depth: SelectThread has other refusal paths (context mismatch, drained run
        // queues), and there's a fire-before-park race where the timer fires and its resume is
        // consumed before we suspend. Either way we're still executing with a positive suspension
        // count, so revert it fully instead of leaving an inflated counter or a stale timer that
        // could half-wake a future sleep.
        const bool timerStillPending = SleepTimerIsPending(currentThread);
        const int32_t suspendCount =
            static_cast<int32_t>(::Memory::Read32(currentThread + kThreadSuspendOffset));
        if (suspendCount > 0) {
            CancelSleepTimer(currentThread);
            ClearOutstandingPark(currentThread);
            ::Memory::Write32(currentThread + kThreadSuspendOffset,
                              static_cast<uint32_t>(suspendCount - 1));
            if (suspendCount == 1) {
                if (::Memory::Read16(currentThread + kThreadStateOffset) == kThreadStateReady &&
                    ::Memory::Read32(currentThread + kThreadQueueOffset) == 0) {
                    ::Memory::Write16(currentThread + kThreadStateOffset, kThreadStateRunning);
                }
                if (Fiber::GuestFiberManager::IsInitialized() &&
                    Fiber::GuestFiberManager::HasFiber(currentThread)) {
                    Fiber::GuestFiberManager::ResumeGuestThread(currentThread);
                }
            }
            thread_local std::unordered_set<uint32_t> reportedFailedTickParks;
            if (reportedFailedTickParks.insert(currentThread).second) {
                RT_LOG(RT_TAG_OS) << "OSSleepTicks: thread 0x" << std::hex << currentThread
                          << std::dec << " failed to switch away while parking; "
                          << "reverted the suspension and busy-returned." << std::endl;
            }
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSSleepTicks", e);
    }

    OS__RestoreInterrupts_801a65d4(irqState);
}

namespace {
// OSSleepThread must reach SelectThread with the scheduler-disable count at zero, or the thread
// relinks into the wait queue a second time and OSWakeupThread livelocks. The SDK guarantees this;
// deferred guest-callback batches (VI retrace, alarms) can raise the count and violate it.
bool SchedulerCanSwitchAway()
{
    return ::Memory::Read32(kSchedulerIdleFlagAddr) == 0;
}

// A blocking call from a non-switchable region can't park, so the caller's retry loop spins; that's
// survivable if host-side work (alarm, IOS completion) eventually satisfies the wait, terminal if
// not. Report once per wait queue and escalate if a site stays wedged long enough to read as a freeze.
void ReportUnparkableSleep(uint32_t queuePtr, uint32_t thread)
{
    using Clock = std::chrono::steady_clock;

    // thread_local rather than static: guest scheduling normally runs on one
    // host thread, but this is reachable from the host frame loop too and a
    // diagnostic must not be the thing that introduces a data race.
    thread_local std::unordered_set<uint32_t> reportedQueues;
    thread_local uint32_t wedgedQueue = 0;
    thread_local Clock::time_point wedgedSince{};
    thread_local bool escalated = false;

    const uint32_t disableCount = ::Memory::Read32(kSchedulerIdleFlagAddr);

    if (reportedQueues.insert(queuePtr).second) {
        RT_LOG(RT_TAG_OS) << "OSSleepThread: thread 0x" << std::hex << thread
                  << " tried to block on wait queue 0x" << queuePtr
                  << std::dec << " but the scheduler could not switch away";
        if (disableCount != 0) {
            std::cerr << " (OSDisableScheduler nesting count " << disableCount << ")";
        }
        std::cerr << ". Not parking - the caller retries instead of corrupting"
                     " the wait queue." << std::endl;
    }

    const auto now = Clock::now();
    if (queuePtr != wedgedQueue) {
        wedgedQueue = queuePtr;
        wedgedSince = now;
        escalated = false;
        return;
    }

    constexpr auto kWedgeThreshold = std::chrono::seconds(5);
    if (!escalated && now - wedgedSince >= kWedgeThreshold) {
        escalated = true;
        RT_LOG(RT_TAG_OS) << "OSSleepThread: wait queue 0x" << std::hex << queuePtr
                  << " has been unsatisfiable for " << std::dec
                  << std::chrono::duration_cast<std::chrono::seconds>(now - wedgedSince).count()
                  << "s from a non-switchable region. The guest callback that is"
                     " blocking here must not block, or the deferred-callback"
                     " scope around it is wrong." << std::endl;
    }
}
} // namespace

// seam-wake fire total (the [seamwake] fire counter in OSSleepThread
// below). File scope since the durability build (DRAIN-DURABILITY-STUDY
// Q4) so the Fix 2 present hook's health line in hle/vi.cpp can read it
// through OS_HLE_SeamWakeFires(); the seam pass increments it exactly
// where it always did.
static uint32_t s_seamWakeFires = 0;
uint32_t OS_HLE_SeamWakeFires()
{
    return s_seamWakeFires;
}

// OSSleepThread (0x801b5c90)
// Puts the current thread to sleep on a specified wait queue.
extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t queuePtr = cpu->gpr[3];
    
    if (queuePtr == 0) {
        RT_LOG(RT_TAG_OS) << "OSSleepThread: null queue pointer!" << std::endl;
        return;
    }

    const int32_t irqState = OS__DisableInterrupts_801a65ac();
    
    try {
        uint32_t currentThread = ::Memory::Read32(kOSRunningContextAddr);
        
        // If no current thread is set but we have a default thread, use that
        // This can happen during early boot before the scheduler has switched threads
        if (currentThread == 0) {
            currentThread = kDefaultThreadContextAddr;
            if (!Memory::Contains(currentThread, 4)) {
                RT_LOG(RT_TAG_OS) << "OSSleepThread: no current thread and default thread not valid!" << std::endl;
                OS__RestoreInterrupts_801a65d4(irqState);
                return;
            }
            // Set the default thread as current (both running and context)
            ::Memory::Write32(kOSRunningContextAddr, currentThread);
            OS__SetCurrentContext_801a1e70(currentThread);
            
            // Also register as a fiber if not already
            if (Fiber::GuestFiberManager::IsInitialized() && 
                !Fiber::GuestFiberManager::HasFiber(currentThread)) {
                RT_LOG(RT_TAG_OS) << "OSSleepThread: registering default thread 0x"
                          << std::hex << currentThread << " as fiber" << std::dec << std::endl;
                Fiber::GuestFiberManager::RegisterMainThreadAsFiber(currentThread, cpu);
            }
        }
        
        // See SchedulerCanSwitchAway: parking is only safe when SelectThread is
        // permitted to switch. Otherwise leave the thread RUNNING and the wait
        // queue untouched and let the caller's retry loop re-test its condition.
        if (!SchedulerCanSwitchAway()) {
            ReportUnparkableSleep(queuePtr, currentThread);
            OS__RestoreInterrupts_801a65d4(irqState);
            return;
        }

        // Mark thread as WAITING
        ::Memory::Write16(currentThread + 0x2C8u, 4);

        // Store the queue we're waiting on
        ::Memory::Write32(currentThread + 0x2DCu, queuePtr);

        // Insert into wait queue (sorted by priority). The shared helper also
        // re-writes the queue pointer stored above and, for a run-queue
        // address, publishes the pending bit - neither applies to a wait queue.
        const int32_t ourPrio = static_cast<int32_t>(::Memory::Read32(currentThread + 0x2D0u));
        InsertThreadIntoQueueByPriority(queuePtr, currentThread, ourPrio);

        // Mark fiber as waiting
        if (Fiber::GuestFiberManager::IsInitialized()) {
            Fiber::GuestFiberManager::SuspendGuestThread(currentThread);
        }

        // tracewake (park seam trace, Nov 16, 2026; PARK-STUDY.md Q4(d)): the thread
        // is linked into the queue and about to suspend. One line per park on the
        // display-sync queue, with the sync snapshot and the guest retrace count
        // at park time: a park whose snapshot diff is already out of the wait set
        // {-1, 2} is a park on a stale latch - the lost-wakeup signature when it
        // follows a wake-to-empty line flagged inrestore=1.
        if (OS_HLE_TraceWakeArmed() && queuePtr == kTraceWakeQueueAddr) {
            static int sleepN = 0;
            if (sleepN++ < 100000) {
                const TraceWakeSync snap = OS_HLE_TraceWakeReadSync();
                RT_LOGF(RT_TAG_OS,
                        "[wake] sleep q=0x%08X thr=0x%08X lr=0x%08X A=%u B=%u arm=%u latch=%u guest=%u d4=%u b8=%u e0=%u flag=%u\n",
                        queuePtr, currentThread, cpu->lr,
                        snap.counterA, snap.counterB, snap.arm, snap.latch, snap.guest,
                        snap.d4, snap.b8, snap.e0, snap.flag);
            }
        }

        // seamfix (SEAMFIX-DESIGN.md element 1): deliver-after-link. The
        // thread is linked into its wait queue (state WAITING, queue word
        // set) but SelectThread has not run, so run the scheduler idle
        // pump's delivery set once, here, on the sleeper's context: the held
        // GX PE-interrupt class, due alarms + the general callback drain,
        // then due retraces. A delivery that wakes this thread - its
        // completion was already queued when it slept - pops it through
        // WakeupThreadQueue; the SelectThread tail below then re-selects
        // it through the normal path and the sleep returns instead of
        // suspending. That is the position the hardware interrupt occupies
        // when it lands after the link, and it is what the convicted
        // display-sync park (parkwatch's target) can never produce for
        // itself once the only submitter has slept. The pass runs inside
        // the deferred-callback scope: a delivered callback that blocks
        // busy-returns (SchedulerCanSwitchAway above), WakeupThreadQueue's
        // immediate reschedule no-ops, so no fiber switch can occur inside
        // the pass. The sleeper's register file is saved and restored, so
        // the pass is invisible in its context either way. Sleep timers
        // and the audio poll are deliberately excluded: resuming other
        // sleepers re-enters SelectThread mid-pass, and feeder manufacture
        // is the stall service's domain, not the park seam's. Inert unless
        // SCOOBY_SEAMFIX is set.
        if (OS_HLE_SeamFixArmed() && !DetClock::Enabled() &&
            !VI_HLE_IsAdvancingRetrace() && !OS_HLE_InRestoreDelivery() &&
            currentThread != kIdleThreadContextAddr) {
            const uint32_t guestBefore = OS_HLE_TraceWakeReadSync().guest;
            const CpuContext savedCpu = *cpu;
            OS_HLE_BeginDeferredGuestCallbacks();
            const bool gxDelivered = DvdProcessPendingGxCallbacks(cpu);
            const bool alarmDelivered = OsHleInternal::ProcessAlarmQueue(cpu, 8);
            VI_HLE_PollRetrace(cpu);
            OS_HLE_EndDeferredGuestCallbacks();
            *cpu = savedCpu;
            const bool woke =
                ::Memory::Read16(currentThread + kThreadStateOffset) != kThreadStateWaiting ||
                ::Memory::Read32(currentThread + kThreadQueueOffset) != queuePtr;
            static uint32_t seamPasses = 0;
            static uint32_t seamWakes = 0;
            if (woke) {
                ++seamWakes;
            }
            if (gxDelivered || alarmDelivered ||
                OS_HLE_TraceWakeReadSync().guest != guestBefore || woke) {
                ++seamPasses;
                if (seamPasses <= 100000) {
                    RT_LOGF(RT_TAG_OS,
                            "[seamfix] post-link delivery q=0x%08X thr=0x%08X gx=%d alarm=%d retrace=%u->%u woke=%d\n",
                            queuePtr, currentThread, gxDelivered ? 1 : 0,
                            alarmDelivered ? 1 : 0, guestBefore,
                            OS_HLE_TraceWakeReadSync().guest, woke ? 1 : 0);
                }
                if (woke) {
                    RT_LOGF(RT_TAG_OS,
                            "[seamfix] woke thr=0x%08X on q=0x%08X (post-link delivery #%u; state=%u queue=0x%08X)\n",
                            currentThread, queuePtr, seamWakes,
                            static_cast<unsigned>(::Memory::Read16(currentThread + kThreadStateOffset)),
                            ::Memory::Read32(currentThread + kThreadQueueOffset));
                }
            }

            // seam-wake (CLIFF-STUDY.md Q4 rank 1): the delivery pass above
            // can strand the display-sync gate with either half charged -
            // the draw-done half delivered into AA4's token-match exit
            // (arm set, no advancer run), or the retrace half spent by
            // BB0's unconditional latch clear one retrace before the arm
            // landed - because the two halves are charged in different
            // passes and every producer of the missing half is downstream
            // of the advancer, whose trigger supply is then dead. No
            // delivery can repair that state: the held deque is empty
            // and the gate is guest memory, not a queued item. But the
            // waiter's post-sleep path presents unconditionally
            // (PARK-STUDY-2.md Q3), so one targeted wake while the thread
            // is still linked restarts the chain from either stranded
            // sub-state - parkwatch's proven actuation (714/714 and
            // 151/151 fires across two boots) moved into the pass, at
            // delivery speed instead of the detector's ~2 s cadence.
            // Scoped exactly: this queue only, the sleeper still linked
            // on it after the delivery steps (a delivery wake above
            // already unlinked it - never fire twice), and the sync
            // snapshot reading pipeline-full ((B - A) mod 3 == 2, the
            // waiter's own legitimate-sleep condition). Any other state
            // is not this defect and does not fire. Armed with seamfix
            // (this whole pass is); the [seamwake] line carries the
            // snapshot and the outcome so a fire off pipeline-full, or
            // one the log shows was not followed by frame resumption,
            // is loud (the study's falsifier). The SelectThread tail
            // below re-selects the woken thread through the normal path.
            if (queuePtr == kTraceWakeQueueAddr &&
                ::Memory::Read16(currentThread + kThreadStateOffset) == kThreadStateWaiting &&
                ::Memory::Read32(currentThread + kThreadQueueOffset) == queuePtr) {
                const TraceWakeSync snap = OS_HLE_TraceWakeReadSync();
                const int32_t diff = static_cast<int32_t>(snap.counterB) -
                                     static_cast<int32_t>(snap.counterA);
                if (((diff % 3) + 3) % 3 == 2) {
                    OS_HLE_WakeupThreadNoReschedule(cpu, queuePtr);
                    const bool seamWoke =
                        ::Memory::Read16(currentThread + kThreadStateOffset) != kThreadStateWaiting ||
                        ::Memory::Read32(currentThread + kThreadQueueOffset) != queuePtr;
                    ++s_seamWakeFires;
                    if (s_seamWakeFires <= 100000) {
                        RT_LOGF(RT_TAG_OS,
                                "[seamwake] fire #%u q=0x%08X thr=0x%08X A=%u B=%u arm=%u latch=%u guest=%u result=%s\n",
                                s_seamWakeFires, queuePtr, currentThread,
                                snap.counterA, snap.counterB, snap.arm, snap.latch,
                                snap.guest, seamWoke ? "woke" : "still-linked");
                    }
                }
            }
        }

        // Set reschedule flag and switch threads
        ::Memory::Write32(kSchedulerReschedCounterAddr, 1);
        
        // Match the original SDK behavior: sleep yields via SelectThread(0).
        cpu->gpr[3] = 0;
        SelectThread_801a9c08(cpu);

        // Defence in depth: SelectThread has other paths that return without switching (context
        // mismatch, uninitialised thread system, a run queue that drained mid-enqueue). None of
        // them may leave this thread WAITING and linked, or the caller's retry loop inserts the same
        // node twice; a real switch resumes here already RUNNING with the queue pointer cleared, so
        // this block never fires there.
        if (::Memory::Read16(currentThread + kThreadStateOffset) == kThreadStateWaiting &&
            ::Memory::Read32(currentThread + kThreadQueueOffset) == queuePtr) {
            // Reverse of the priority-sorted insert above, so a thread is never
            // left WAITING and linked while its caller keeps running on the
            // same stack.
            UnlinkGuestListNode(currentThread, kThreadNextOffset, kThreadPrevOffset, queuePtr,
                                queuePtr + 4u);
            ::Memory::Write32(currentThread + kThreadQueueOffset, 0);
            ::Memory::Write16(currentThread + kThreadStateOffset, kThreadStateRunning);
            if (Fiber::GuestFiberManager::IsInitialized() &&
                Fiber::GuestFiberManager::HasFiber(currentThread)) {
                Fiber::GuestFiberManager::ResumeGuestThread(currentThread);
            }
            ReportUnparkableSleep(queuePtr, currentThread);
        }

    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSSleepThread", e);
    }
    
    OS__RestoreInterrupts_801a65d4(irqState);
}
PPC_NATIVE_OVERRIDE_VOID(80108708, OSSleepThread_HLE_801aa9b8, (CpuContext* ctx), (ctx));
