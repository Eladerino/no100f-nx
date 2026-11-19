// NO100F_PROFILE=<start s>:<length s> (Windows): a sampling profiler for the game thread. From
// <start> seconds after the first presented frame, for <length> seconds, the thread that presents
// frames is suspended about a thousand times a second and its instruction pointer recorded. At the
// end, samples are mapped back to the translated guest function whose code they fall in (by each
// translated function's host entry point) and the top entries are logged with their guest address,
// so a slow scene can be traced to the game code (or the runtime and aurora code) it spends time in.
#include "guest_profiler.h"

#include "abi_bridge.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#ifdef __SWITCH__
#include <switch.h>
#endif

extern "C" {
extern const uint32_t kGuestMapSymbolCount;
extern const uint32_t kGuestMapSymbolAddresses[];
extern const char* const kGuestMapSymbolNames[];
}

namespace GuestProfiler {
namespace {

#if defined(_WIN32) || defined(__SWITCH__)
struct Entry {
    uintptr_t host;
    uint32_t guest;
};

const char* GuestName(uint32_t address) {
    const uint32_t* begin = kGuestMapSymbolAddresses;
    const uint32_t* end = kGuestMapSymbolAddresses + kGuestMapSymbolCount;
    const uint32_t* it = std::lower_bound(begin, end, address);
    return it != end && *it == address ? kGuestMapSymbolNames[it - begin] : "";
}
#endif

#ifdef _WIN32

void Report(const std::unordered_map<uintptr_t, uint32_t>& samples, const std::vector<std::vector<uintptr_t>>& stacks,
            uint32_t total) {
    // Every translated function's host entry point, sorted, to find which one a sample falls in.
    std::vector<Entry> entries;
    for (const auto& [lo, hi] : {std::pair{0x80011B78u, 0x80319180u}, std::pair{0x80BF691Cu, 0x80C5A648u}}) {  // guest code ranges to index
        for (uint32_t address = lo; address < hi; address += 4) {
            if (const TranslatedFunctionInfo* info = TranslatedFunctionRegistry::FindByAddressPtr(address);
                info != nullptr && info->entryPoint != nullptr && info->address == address) {
                entries.push_back({reinterpret_cast<uintptr_t>(info->entryPoint), address});
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
    const auto exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exeBase + dos->e_lfanew);
    const uintptr_t exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;

    // A host address as a translated guest function, or exe+RVA (resolve with llvm-nm), or [dll].
    const auto describe = [&](uintptr_t rip) -> std::string {
        if (rip >= exeBase && rip < exeEnd) {
            auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            // Translated code sits in its own range: past the last entry point it is runtime code.
            if (it != entries.begin() && it != entries.end()) {
                --it;
                char text[160];
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                return text;
            }
            char text[40];
            std::snprintf(text, sizeof(text), "exe+%llX", static_cast<unsigned long long>(rip - exeBase));
            return text;
        }
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &module)) {
            GetModuleFileNameA(module, path, MAX_PATH);
        }
        const char* slash = std::strrchr(path, '\\');
        return std::string("[") + (slash ? slash + 1 : path) + "]";
    };
    // Raw stacks (exe RVAs, "-" for other modules) for offline symbolization: nm the exe, see AGENTS.md.
    if (FILE* raw = std::fopen("profile_stacks.txt", "w")) {
        for (const auto& stack : stacks) {
            for (const uintptr_t rip : stack) {
                if (rip >= exeBase && rip < exeEnd) {
                    std::fprintf(raw, "%llX ", static_cast<unsigned long long>(rip - exeBase));
                } else {
                    std::fputs("- ", raw);
                }
            }
            std::fputc('\n', raw);
        }
        std::fclose(raw);
    }
    // Where each sample's stack enters system code from ours: the first frames inside the exe.
    std::unordered_map<std::string, uint32_t> chains;
    for (const auto& stack : stacks) {
        std::string chain;
        int exeFrames = 0;
        for (const uintptr_t rip : stack) {
            if (rip >= exeBase && rip < exeEnd) {
                chain += (chain.empty() ? "" : " < ") + describe(rip);
                if (++exeFrames == 5) break;
            } else if (exeFrames > 0) {
                break;
            }
        }
        if (stack.empty() || (stack[0] >= exeBase && stack[0] < exeEnd)) {
            continue;  // already in our code: the leaf table covers it
        }
        chains[describe(stack[0]) + " from " + (chain.empty() ? "?" : chain)]++;
    }
    std::vector<std::pair<std::string, uint32_t>> sortedChains(chains.begin(), chains.end());
    std::sort(sortedChains.begin(), sortedChains.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sortedChains.size() && i < 25; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile chain: %5.1f%%  %s\n", 100.0 * sortedChains[i].second / total,
                sortedChains[i].first.c_str());
    }

    std::unordered_map<std::string, uint32_t> byName;
    for (const auto& [rip, count] : samples) {
        std::string key;
        if (rip < exeBase || rip >= exeEnd) {
            HMODULE module = nullptr;
            char path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(rip), &module)) {
                GetModuleFileNameA(module, path, MAX_PATH);
            }
            const char* slash = std::strrchr(path, '\\');
            key = std::string("[") + (slash ? slash + 1 : path) + "]";
        } else {
            auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            if (it == entries.begin()) {
                key = "[exe, before translated code]";
            } else {
                --it;
                char text[160];
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                key = text;
            }
        }
        byName[key] += count;
    }
    std::vector<std::pair<std::string, uint32_t>> sorted(byName.begin(), byName.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    RT_LOGF(RT_TAG_RUNTIME, "profile: %u samples, %zu translated functions\n", total, entries.size());
    for (size_t i = 0; i < sorted.size() && i < 60; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile: %5.1f%%  %s\n", 100.0 * sorted[i].second / total, sorted[i].first.c_str());
    }
}

void Run(HANDLE thread, double startSeconds, double lengthSeconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
    std::unordered_map<uintptr_t, uint32_t> samples;
    std::vector<std::vector<uintptr_t>> stacks;
    uint32_t total = 0;
    auto next = std::chrono::steady_clock::now();
    const auto end = next + std::chrono::duration<double>(lengthSeconds);
    while (std::chrono::steady_clock::now() < end) {
        // About 1000 samples a second; Sleep() is too coarse for that, so wait by yielding.
        next += std::chrono::microseconds(1000);
        while (std::chrono::steady_clock::now() < next) {
            std::this_thread::yield();
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            // No heap use while the game thread is suspended: it may hold the heap lock (deadlock).
            uintptr_t frames[24];
            int depth = 0;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                // The call stack, by the unwind data every x64 function carries.
                for (; depth < 24 && context.Rip != 0; ++depth) {
                    frames[depth] = static_cast<uintptr_t>(context.Rip);
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                    if (function == nullptr) {
                        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);  // a leaf: return address on top
                        context.Rsp += 8;
                        continue;
                    }
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                     &establisher, nullptr);
                }
            }
            ResumeThread(thread);
            if (depth > 0) {
                ++samples[frames[0]];
                ++total;
                stacks.emplace_back(frames, frames + depth);
            }
        }
    }
    Report(samples, stacks, total);
}
#endif

#ifdef __SWITCH__
// The same sampler on Horizon (UserData/profile.txt "<start s>:<length s>"): a thread on another core
// pauses the game thread about a thousand times a second (svcSetThreadActivity, which returns once the
// thread is off every core), reads its PC and LR (svcGetThreadContext3: only for a paused thread of the
// calling process) and resumes it. No heap use while it is paused (it may hold the heap lock): samples
// go into a buffer sized before the window. Cost while on: two SVCs and a context copy per sample, tens
// of microseconds a millisecond. A PC is mapped to the translated guest function it falls in, else
// printed as elf+offset (aarch64-none-elf-addr2line -f -C -e no100f-nx.elf <offset>).
struct SwitchSample {
    uint64_t pc, lr;
};

void SwitchReport(const SwitchSample* samples, uint32_t total, uintptr_t textBase, uintptr_t textEnd) {
    std::vector<Entry> entries;
    for (const auto& [lo, hi] : {std::pair{0x80011B78u, 0x80319180u}, std::pair{0x80BF691Cu, 0x80C5A648u}}) {
        for (uint32_t address = lo; address < hi; address += 4) {
            // Native (HLE) overrides are runtime functions, not translated bodies: indexing them made every
            // runtime function linked after one count as that guest function (the Nov 14 route profile's
            // "GXEndDisplayList 14.3%" was the gather-pipe HLE code after No100f_GXEndDisplayList).
            if (const TranslatedFunctionInfo* info = TranslatedFunctionRegistry::FindByAddressPtr(address);
                info != nullptr && info->entryPoint != nullptr && info->address == address &&
                info->kind != FunctionKind::Native) {
                entries.push_back({reinterpret_cast<uintptr_t>(info->entryPoint), address});
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
    // Translated code sits in its own range of the image: from the first entry point to the last.
    const uintptr_t translatedLo = entries.empty() ? 0 : entries.front().host;
    const uintptr_t translatedHi = entries.empty() ? 0 : entries.back().host + 0x10000;
    const auto describe = [&](uintptr_t pc) -> std::string {
        char text[160];
        if (pc >= translatedLo && pc < translatedHi) {
            auto it = std::upper_bound(entries.begin(), entries.end(), pc,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            if (it != entries.begin()) {
                --it;
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                return text;
            }
        }
        if (pc >= textBase && pc < textEnd) {
            std::snprintf(text, sizeof(text), "elf+%llX", static_cast<unsigned long long>(pc - textBase));
            return text;
        }
        return "[outside the image: kernel/libnx stub or data]";
    };
    std::unordered_map<std::string, uint32_t> leaf, pairs;
    uint32_t translated = 0;
    for (uint32_t i = 0; i < total; ++i) {
        const std::string name = describe(samples[i].pc);
        ++leaf[name];
        translated += samples[i].pc >= translatedLo && samples[i].pc < translatedHi ? 1 : 0;
        // LR names the caller exactly only while the PC is in a leaf; elsewhere it is the last call's
        // return address in the current function, still a useful hint.
        ++pairs[name + " <- " + describe(samples[i].lr)];
    }
    const auto top = [&](const std::unordered_map<std::string, uint32_t>& counts, const char* tag, size_t n) {
        std::vector<std::pair<std::string, uint32_t>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < sorted.size() && i < n; ++i) {
            RT_LOGF(RT_TAG_RUNTIME, "%s: %5.1f%%  %s\n", tag, 100.0 * sorted[i].second / total, sorted[i].first.c_str());
        }
    };
    RT_LOGF(RT_TAG_RUNTIME, "profile: %u samples of the game thread, %.1f%% in translated code, %zu translated functions\n",
            total, total ? 100.0 * translated / total : 0.0, entries.size());
    top(leaf, "profile", 60);
    top(pairs, "profile pair", 30);
    // The exact PCs too, unmapped (resolve offline: aarch64-none-elf-addr2line -f -C -i -e no100f-nx.elf):
    // a translated-function label is only a nearest-preceding-entry guess, these are not.
    std::unordered_map<std::string, uint32_t> raw;
    for (uint32_t i = 0; i < total; ++i) {
        char text[40];
        const uintptr_t pc = samples[i].pc;
        if (pc >= textBase && pc < textEnd) {
            std::snprintf(text, sizeof(text), "elf+%llX", static_cast<unsigned long long>(pc - textBase));
        } else {
            std::snprintf(text, sizeof(text), "outside");
        }
        ++raw[text];
    }
    top(raw, "profile pc", 60);
}

void SwitchRun(Handle thread, double startSeconds, double lengthSeconds) {
    svcSleepThread(static_cast<int64_t>(startSeconds * 1e9));
    const uint32_t capacity = static_cast<uint32_t>(std::min(lengthSeconds, 120.0) * 1100.0) + 16;
    std::vector<SwitchSample> samples(capacity);
    uint32_t total = 0, failed = 0;
    Result firstError = 0;
    const uint64_t end = armGetSystemTick() + armNsToTicks(static_cast<uint64_t>(lengthSeconds * 1e9));
    RT_LOGF(RT_TAG_RUNTIME, "profile: sampling the game thread for %.0f s\n", lengthSeconds);
    // phasetime (PHASETIME design, Nov 19, 2026): 500 Hz sampling while SCOOBY_PHASETIME is
    // armed (2 ms spacing); the 1 ms default is unchanged otherwise.
    static const bool phaseTime = std::getenv("SCOOBY_PHASETIME") != nullptr;
    const uint64_t profSpacingNs = phaseTime ? 2000000 : 1000000;
    while (armGetSystemTick() < end && total < capacity) {
        svcSleepThread(static_cast<int64_t>(profSpacingNs));  // 1 ms, 2 ms while phasetime is armed
        Result rc = svcSetThreadActivity(thread, ThreadActivity_Paused);
        if (R_SUCCEEDED(rc)) {
            ThreadContext context{};
            rc = svcGetThreadContext3(&context, thread);
            svcSetThreadActivity(thread, ThreadActivity_Runnable);
            if (R_SUCCEEDED(rc)) {
                samples[total++] = {context.pc.x, context.lr};
                continue;
            }
        }
        if (failed++ == 0) firstError = rc;
        if (failed == 50 && total == 0) break;  // not allowed here: say so once and stop
    }
    if (failed) {
        RT_LOGF(RT_TAG_RUNTIME, "profile: %u samples failed (first rc 0x%X)\n", failed, static_cast<unsigned>(firstError));
    }
    MemoryInfo info{};
    u32 page = 0;
    uintptr_t textBase = 0, textEnd = 0;
    if (R_SUCCEEDED(svcQueryMemory(&info, &page, reinterpret_cast<uintptr_t>(&SwitchReport)))) {
        textBase = static_cast<uintptr_t>(info.addr);
        textEnd = textBase + static_cast<uintptr_t>(info.size);
    }
    if (total) SwitchReport(samples.data(), total, textBase, textEnd);
}
#endif

} // namespace

namespace {

// Spinwatch state (SPINNER-PC-STUDY Q3): the game thread's handle, captured at the first
// present while SCOOBY_SPINWATCH is armed (the capture is in OnFramePresented below).
// Written once by the game thread, read by the safepoint gate's shot thread in hle/vi.cpp.
// The runtime records no main-thread handle anywhere else - this file's profile capture is
// the tree's only threadGetCurHandle - so spinwatch captures its own here.
#ifdef __SWITCH__
std::atomic<uint32_t> g_spinwatchGameThread{0};
bool SpinwatchArmed() {
    static const bool armed = std::getenv("SCOOBY_SPINWATCH") != nullptr;
    return armed;
}
#endif

} // namespace

uint32_t SpinwatchGameThreadHandle() {
#ifdef __SWITCH__
    return g_spinwatchGameThread.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

uint32_t SpinwatchSampleGameThread(uint32_t handle, SpinwatchPcSample* out, uint32_t capacity,
                                   uint32_t spacingMs, uint32_t* failCount, uint32_t* firstFailRc,
                                   const char** firstFailStage) {
    uint32_t total = 0, failed = 0;
    uint32_t firstError = 0;
    const char* firstStage = "";
#ifdef __SWITCH__
    // The profiler's Switch loop shape (SwitchRun) with fixed parameters: pause the game
    // thread (svcSetThreadActivity returns once it is off every core), read its PC and LR
    // (svcGetThreadContext3: only for a paused thread of the calling process), resume it.
    // The output buffer is the caller's, sized before the window; nothing allocates anywhere
    // in this loop, and nothing runs while the thread is paused beyond the context read and
    // the resume (it may hold the heap lock). First failure's rc and stage are kept; the shot
    // aborts after 8 failed samples (the profiler's repeated-failure discipline).
    while (total < capacity && failed < 8) {
        if (total + failed > 0) {
            svcSleepThread(static_cast<int64_t>(spacingMs) * 1000000);
        }
        Result rc = svcSetThreadActivity(static_cast<Handle>(handle), ThreadActivity_Paused);
        if (R_SUCCEEDED(rc)) {
            ThreadContext context{};
            rc = svcGetThreadContext3(&context, static_cast<Handle>(handle));
            svcSetThreadActivity(static_cast<Handle>(handle), ThreadActivity_Runnable);
            if (R_SUCCEEDED(rc)) {
                out[total++] = {context.pc.x, context.lr};
                continue;
            }
            if (failed++ == 0) {
                firstError = static_cast<uint32_t>(rc);
                firstStage = "context";
            }
        } else {
            if (failed++ == 0) {
                firstError = static_cast<uint32_t>(rc);
                firstStage = "pause";
            }
        }
    }
#else
    (void)handle;
    (void)out;
    (void)capacity;
    (void)spacingMs;
#endif
    if (failCount != nullptr) *failCount = failed;
    if (firstFailRc != nullptr) *firstFailRc = firstError;
    if (firstFailStage != nullptr) *firstFailStage = firstStage;
    return total;
}

bool SpinwatchResolve(const SpinwatchPcSample* samples, uint32_t count, uint32_t minorityMin,
                      SpinwatchResolution* out) {
    if (out == nullptr) {
        return false;
    }
    *out = SpinwatchResolution{};
    out->total = count;
    if (samples == nullptr || count == 0) {
        return false;
    }
#ifdef __SWITCH__
    // The registry entry table, built exactly as SwitchReport builds it: every translated
    // function's host entry point over the two guest code ranges, Native (HLE) kinds
    // excluded (indexing them made every runtime function linked after one count as that
    // guest function - the Nov 14 route profile's "GXEndDisplayList 14.3%").
    std::vector<Entry> entries;
    for (const auto& [lo, hi] : {std::pair{0x80011B78u, 0x80319180u}, std::pair{0x80BF691Cu, 0x80C5A648u}}) {
        for (uint32_t address = lo; address < hi; address += 4) {
            if (const TranslatedFunctionInfo* info = TranslatedFunctionRegistry::FindByAddressPtr(address);
                info != nullptr && info->entryPoint != nullptr && info->address == address &&
                info->kind != FunctionKind::Native) {
                entries.push_back({reinterpret_cast<uintptr_t>(info->entryPoint), address});
            }
        }
    }
    if (entries.empty()) {
        return false;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
    // Translated code sits in its own range of the image: from the first entry point to the last.
    const uintptr_t translatedLo = entries.front().host;
    const uintptr_t translatedHi = entries.back().host + 0x10000;
    MemoryInfo info{};
    u32 page = 0;
    uintptr_t textBase = 0, textEnd = 0;
    if (R_SUCCEEDED(svcQueryMemory(&info, &page, reinterpret_cast<uintptr_t>(&SpinwatchResolve)))) {
        textBase = static_cast<uintptr_t>(info.addr);
        textEnd = textBase + static_cast<uintptr_t>(info.size);
    }
    // One address resolved: the grouping key (the guest address for translated code, the raw
    // PC itself otherwise - a spin in runtime code sits on one PC or a tight loop, so the raw
    // PC is the honest vote), the guest address and name, and the elf+offset form.
    struct AddrInfo {
        uint64_t key = 0;
        uint32_t guest = 0;
        const char* name = "";
        uint64_t elfOffset = 0;
    };
    const auto infoOf = [&](uint64_t raw) {
        AddrInfo a;
        const uintptr_t pc = static_cast<uintptr_t>(raw);
        if (pc >= textBase && pc < textEnd) {
            a.elfOffset = pc - textBase;
        }
        a.key = raw;
        if (pc >= translatedLo && pc < translatedHi) {
            auto it = std::upper_bound(entries.begin(), entries.end(), pc,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            if (it != entries.begin()) {
                --it;
                a.guest = it->guest;
                a.name = GuestName(it->guest);
                a.key = it->guest;
            }
        }
        return a;
    };
    struct Group {
        uint64_t key = 0;
        uint32_t guest = 0;
        const char* name = "";
        uint64_t elfOffset = 0;
        uint32_t samples = 0;
    };
    const auto addTo = [](std::vector<Group>& groups, const AddrInfo& a) {
        for (Group& g : groups) {
            if (g.key == a.key) {
                ++g.samples;
                return;
            }
        }
        groups.push_back({a.key, a.guest, a.name, a.elfOffset, 1});
    };
    std::vector<Group> pcGroups;
    std::vector<uint64_t> pcKeys(count);
    for (uint32_t i = 0; i < count; ++i) {
        const AddrInfo a = infoOf(samples[i].pc);
        pcKeys[i] = a.key;
        addTo(pcGroups, a);
    }
    const Group* modal = &pcGroups.front();
    for (const Group& g : pcGroups) {
        if (g.samples > modal->samples) {
            modal = &g;
        }
    }
    // The modal LR among the modal PC's samples (LR names the caller exactly only while the
    // PC is in a leaf; elsewhere it is a hint, as in the profiler's own report).
    std::vector<Group> lrGroups;
    for (uint32_t i = 0; i < count; ++i) {
        if (pcKeys[i] == modal->key) {
            addTo(lrGroups, infoOf(samples[i].lr));
        }
    }
    const Group* modalLr = lrGroups.empty() ? nullptr : &lrGroups.front();
    for (const Group& g : lrGroups) {
        if (modalLr == nullptr || g.samples > modalLr->samples) {
            modalLr = &g;
        }
    }
    out->modalPc = {modal->guest, modal->name, modal->samples, modal->elfOffset};
    if (modalLr != nullptr) {
        out->modalLr = {modalLr->guest, modalLr->name, modalLr->samples, modalLr->elfOffset};
    }
    std::vector<const Group*> minorities;
    for (const Group& g : pcGroups) {
        if (g.key != modal->key && g.samples >= minorityMin) {
            minorities.push_back(&g);
        }
    }
    std::sort(minorities.begin(), minorities.end(),
              [](const Group* a, const Group* b) { return a->samples > b->samples; });
    for (size_t i = 0; i < minorities.size() && i < 3; ++i) {
        out->minorities[i] = {minorities[i]->guest, minorities[i]->name, minorities[i]->samples,
                              minorities[i]->elfOffset};
        ++out->minorityCount;
    }
    return true;
#else
    (void)samples;
    (void)minorityMin;
    return false;
#endif
}

void OnFramePresented() {
#ifdef __SWITCH__
    // Spinwatch (SPINNER-PC-STUDY Q3): while SCOOBY_SPINWATCH is armed, capture this thread's
    // handle at the first present, for the gate-open shot in hle/vi.cpp. This is the game
    // thread (it presents - the same identity the profile.txt capture below relies on) and its
    // handle is valid in any thread of the process, so the shot thread can pause-and-sample
    // it at the wedge. Captured here because the present path is the one place this thread
    // provably runs before any wedge can form.
    if (SpinwatchArmed() && g_spinwatchGameThread.load(std::memory_order_relaxed) == 0) {
        g_spinwatchGameThread.store(threadGetCurHandle(), std::memory_order_relaxed);
    }
    static bool switchStarted = false;
    if (!switchStarted) {
        switchStarted = true;
        double start = 0, length = 0;
        if (FILE* f = std::fopen("/switch/no100f-nx/UserData/profile.txt", "r")) {
            char text[64] = {};
            const bool read = std::fgets(text, sizeof(text), f) != nullptr;
            std::fclose(f);
            if (read && std::sscanf(text, "%lf:%lf", &start, &length) == 2 && length > 0) {
                // This is the game thread (it presents): its handle is valid in any thread of the process.
                const Handle thread = threadGetCurHandle();
                std::thread(SwitchRun, thread, start, length).detach();
            }
        }
    }
#endif
#ifdef _WIN32
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    const char* value = std::getenv("NO100F_PROFILE");
    // Also read "<start>:<length>" from profile.txt beside the executable (the test loop's switch).
    static char fileValue[64] = {};
    if (value == nullptr) {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string path(exePath);
        path = path.substr(0, path.find_last_of("\\/") + 1) + "profile.txt";
        if (FILE* f = std::fopen(path.c_str(), "r")) {
            if (std::fgets(fileValue, sizeof(fileValue), f)) value = fileValue;
            std::fclose(f);
        }
    }
    double start = 0, length = 0;
    if (value == nullptr || std::sscanf(value, "%lf:%lf", &start, &length) != 2 || length <= 0) {
        return;
    }
    HANDLE thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::thread(Run, thread, start, length).detach();
#endif
}

} // namespace GuestProfiler
