#pragma once

#include <cstdint>

// NO100F_PROFILE=<start s>:<length s>: sample the game thread and log where its time goes, by
// translated guest function (guest_profiler.cpp; Windows only).
namespace GuestProfiler {
// From the thread that presents frames, at each present: the first call starts the sampler.
void OnFramePresented();

// Spinwatch (SPINNER-PC-STUDY Q3): a one-shot pause-and-sample of the game thread, fired by
// the safepoint gate in hle/vi.cpp at its open. While SCOOBY_SPINWATCH is armed,
// OnFramePresented captures the presenting thread's handle (the game thread: it presents);
// the gate reads it back through SpinwatchGameThreadHandle (0 = never captured).
// SpinwatchSampleGameThread runs the profiler's Switch sampling loop with fixed parameters
// into a caller-owned buffer: it allocates nothing, and nothing runs while the game thread
// is paused beyond the context read and the resume. SpinwatchResolve maps the samples to
// translated guest functions exactly as the profiler's Switch report does (registry entry
// table, Native kinds excluded, translated-range bound) and returns the modal PC function,
// the modal LR among the modal PC's samples, and minority functions.
struct SpinwatchPcSample {
    uint64_t pc = 0;
    uint64_t lr = 0;
};
uint32_t SpinwatchGameThreadHandle();
uint32_t SpinwatchSampleGameThread(uint32_t handle, SpinwatchPcSample* out, uint32_t capacity,
                                   uint32_t spacingMs, uint32_t* failCount, uint32_t* firstFailRc,
                                   const char** firstFailStage);
struct SpinwatchFunction {
    uint32_t guest = 0;     // translated guest address; 0 when the PC/LR is not translated code
    const char* name = "";  // map symbol name for guest (static storage), "" when none
    uint32_t samples = 0;
    uint64_t elfOffset = 0;  // a representative raw PC/LR as elf+offset (0 when outside the image)
};
struct SpinwatchResolution {
    SpinwatchFunction modalPc{};
    SpinwatchFunction modalLr{};
    SpinwatchFunction minorities[3]{};
    uint32_t minorityCount = 0;
    uint32_t total = 0;
};
bool SpinwatchResolve(const SpinwatchPcSample* samples, uint32_t count, uint32_t minorityMin,
                      SpinwatchResolution* out);
} // namespace GuestProfiler
