// Native-scene Stage 1+2 (60 fps campaign, per STAGE1-DESIGN.md and
// STAGE2-DESK.md): intercept pinned static prop atomics on the Mystic Manor
// route — geometry counts 302/198, texture "rma00001" (route census in the
// s1e/s1diag logs) — parse each RpGeometry once into per-pointer cached
// host geometry, and let the display-list player submit that geometry
// natively at the exact stream position of the prop's own display list.
// Everything else stays on the GX path.
//
// N3 (NATIVE-SCENE-N1-DESIGN.md §3): multi-mesh generalization. The
// single-target pin is now an allowlist table (Wave 1: 15 meshes=1 statics,
// anchor rma00001; verification subjects baseplate_12, 5 meshes, menu
// phase, and obbe0001, 3 meshes, route). Each mesh parses into its own
// packed stream with its own learned DL — the N3 diagnostic proved one DL
// per mesh with no interleaved per-mesh state (Q1 outcome A, Q2 one DL
// per mesh, Q3 no per-mesh bind extension), so conversion submits one
// native draw per mesh DL in stream order and binding stays the immediate
// path's doing. A mesh that fails validation stays GX; it never poisons
// its sibling meshes.
//
// Interception (STAGE1-DESIGN Q1): PPC_NATIVE_OVERRIDE removes the translated
// original, so the render callback 0x801488B8 at the bottom of this file is a
// faithful mechanical transliteration of the translator's own shard body
// (shard_af3097b1a5d24f3f8a8b3ba2.cpp lines 16688-16803), in the same pattern
// as the Stage 0 probe overrides. The only deviation: with the dk_diag key
// "nativescene" on and the atomic passing the pin, an "arm" record is set
// around the (unchanged) RxPipelineExecute call; the guest-side
// GXCallDisplayList override notes the prop's display list inside that window,
// and gx_dl.cpp's player converts that one list's submission to the cached
// packed geometry. Non-target behaviour is the generated code's behaviour,
// line for line.
//
// Geometry layout is this fork's RpGeometry as parsed by the sister project
// (night100 native_scene.cpp geometry()) and confirmed against the PR2 title
// dump's allocation arithmetic: positions/normals hang off morph target 0
// (+0x14/+0x18), colours at +0x30, texcoords at +0x34, indices are u16 strips
// in the mesh header at +0x60. The parse is untrusted until the validation
// battery passes; any failure poisons the geometry for the boot and the GX
// path renders it, exactly as before.

#include "no100f_native_scene.h"

#include "gx_guest_write.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_config.h"

// N5: Aurora bbox walk stats (aurora_bbox_totals / AuroraStageTimerTotals)
// — the walk itself is already instrumented in Aurora
// (g_statBBoxArmedVerts, T6 stage timer); the nativescene PrintCounters
// line reads them through the existing getters instead of adding a second
// hook. aurora_bbox_totals is declared in aurora-main/include/aurora/gfx.h.
// AuroraStageTimerTotals is declared in aurora-main/include/dolphin/gx/
// GXAurora.h; this TU forward-declares it rather than including that
// GX-internal header (same pattern as the GuestToHostPtr forward
// declaration above). Signature verified against GXAurora.h:113 and the
// definition in command_processor.cpp.
extern "C" void AuroraStageTimerTotals(uint64_t* admitNs, uint64_t* resolveNs, uint64_t* uploadNs,
                                       uint64_t* uniformNs, uint64_t* recordNs, uint64_t* bboxNs);
#include <aurora/gfx.h>

// N3a: the DL content digest (XXH3_64bits, same digest the player's DL scan
// cache computes) is the retention check. <xxhash.h> resolves the same way
// it does for gx_dl.cpp (wiicompiled/Dependencies/xxhash).
#include <xxhash.h>

// N3a: GuestToHostPtr is declared in wiicompiled/runtime/src/hle/gx/
// gx_internal.h:321 (global scope); this TU forward-declares it rather than
// including that GX-internal header. Used only for the lazy fallback digest
// inside TryGetPacked (first sighting / uncached path); null = unmapped.
void* GuestToHostPtr(uint32_t addr, size_t len = 0);

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <string>
#include <vector>

// Generated callee, declared directly (runtime TUs get no generated header).
extern "C" void func_801850E4(CpuContext* ctx);

namespace {

// Allowlist (N1 §1.4, N3). Stage 2's single-target pin (rma00001,
// 302/198, 1 mesh) generalized to a table: one row per admitted geometry,
// matched on (countA, countB, meshCount, mesh[0] texture). Identity stays
// per geometry pointer — rows with identical counts+name (owi0009a,
// odfh1103, otrh0009 636/320) are shared by several geometries, and the
// per-pointer cache keeps them apart. [geom+0x24] tracking the mesh count
// was proven by the Nov 16 diag boot pin-scan over 45 geometries. The
// cull-only 636/320 prop (81446F80) shares a row with 81442180 but never
// renders, so it never arms. Names are the per-mesh texture expectation
// where known (baseplate_12's five, from the N3 diagnostic meshes line);
// a null entry means "must resolve to a readable name" (obbe0001 meshes
// 1-2, whose textures the census never saw). Wave 2 "?" chunks (#18/#21)
// and Wave 1P (#14/#15/#7, skin-pending) are NOT in this table.
constexpr uint32_t kMaxMeshes = 16;  // N3 parse cap (census max is 9)

struct AllowlistRow {
    uint32_t countA;  // [geom+0x10]
    uint32_t countB;  // [geom+0x14] numVertices
    uint32_t meshCount;  // [geom+0x24] and mesh-header mesh count
    const char* names[kMaxMeshes];  // per-mesh texture names; only nameCount set
    uint32_t nameCount;
};

constexpr AllowlistRow kAllowlist[] = {
    // Wave 1 (N4a): meshes=1 statics, route census. Rows are shared where
    // counts+name collide; the geometries each row covers are named.
    {204, 105, 1, {"otrh0009"}, 1},   // 8143CA00
    {460, 232, 1, {"otrh0009"}, 1},   // 8143E780
    {636, 320, 1, {"otrh0009"}, 1},   // 81442180 (cull-only 81446F80 shares, never renders)
    {76, 63, 1, {"otre0101"}, 1},     // 814245C0
    {12, 24, 1, {"sign_monster_gallery"}, 1},  // 81425800
    {50, 53, 1, {"olkc0001"}, 1},     // 81423620
    {20, 36, 1, {"owi0009a"}, 1},     // 8141F000, 8141E400, 8141D800, 8141CC00
    {80, 103, 1, {"odfh1103"}, 1},    // 8141FC00, 81421420
    {148, 160, 1, {"skull_key"}, 1},  // 813D00C0
    {302, 198, 1, {"rma00001"}, 1},   // 813D23A0 (Wave 1 anchor, Stage 2 proven)
    {94, 98, 1, {"rhss0001_gc"}, 1},  // 81190D80
    // N3 verification subjects (§3.4): baseplate_12 (menu phase) and
    // obbe0001 (route). Their meshes are what N3 proves.
    {204, 200, 5, {"baseplate_12", "occiliscope2", "ommh0004", "owas2301", "MBus_2"}, 5},  // 8137A4E0
    {1496, 1035, 3, {"obbe0001"}, 1},  // 8142EE20 (meshes 1-2: resolve-only)
};

// Row lookup by counts only (every (countA, countB) pair in the table is
// distinct); the caller then checks meshCount and the mesh[0] name.
const AllowlistRow* FindAllowlistRow(uint32_t countA, uint32_t countB) {
    for (const auto& row : kAllowlist) {
        if (row.countA == countA && row.countB == countB) return &row;
    }
    return nullptr;
}

constexpr uint32_t kExpectedGeometry = 0x813D23A0;  // s1e/s1diag route census; logged, not gated
constexpr uint32_t kVertexBytes = 36;               // pos 12 + nrm 12 + clr 4 + tex0 8

enum Fallback : int {
    kFbPinStructure = 0,  // counts matched but morph/material structure did not
    kFbNameMismatch,      // texture name differs from the learned name
    kFbParseInvalid,      // validation battery failed (geometry poisoned)
    kFbGenChanged,        // guest-write generation moved under a cached parse
    kFbDlUnlearned,       // conversion attempted with no valid cache
    kFbDlDrift,           // learned list's content generation moved; unlearned, will re-learn
    kFbInterpOn,          // frame interpolation enabled: native path self-disables
    kFbAmbiguous,         // legacy slot (Stage 1 single-pointer guard); Stage 2 binds per-pointer, never fires
    kFbMeshParseInvalid,  // N3: one mesh failed validation (that mesh stays GX; logged with its index)
    kFbLearnVertsMismatch,  // N3: a DL in the armed window matched no unlearned mesh's vertex span
    kFbCount
};

std::atomic<uint32_t> g_fallbacks[kFbCount]{};
std::atomic<uint32_t> g_nativeDraws{0};
std::atomic<uint32_t> g_cacheBuilds{0};
std::atomic<uint32_t> g_armCalls{0};
std::atomic<uint32_t> g_windowExtraDl{0};  // informational: window DLs that are not the learned identity
// N3a digest-retention counters (N3-LEARN-RETENTION-DESIGN.md §3.2/§3.5).
// digest_retained: generation moved, digest matched, converted anyway.
// digest_changed: generation moved, digest differed, drifted to GX (genuine
// content change; expected near zero). digest_unavailable: generation moved
// but no digest was available at the decision, so the old drift behaviour
// was taken (expected zero; nonzero means the plumbing failed).
std::atomic<uint32_t> g_digestRetained{0};
std::atomic<uint32_t> g_digestChanged{0};
std::atomic<uint32_t> g_digestUnavailable{0};
// digest_changed log lines are capped (first 16 individually, then counted
// only); a genuine content change is the interesting event.
std::atomic<uint32_t> g_digestChangedLogged{0};
// N3b (§3.5): DLs whose array staging was skipped at a submission site.
std::atomic<uint32_t> g_stagingSkipped{0};

// N8 C5: per-geometry recurrence (see N8-COUNTER-BUILD-LOG.md). The conversion
// site cannot attribute uncatalogued DLs to geometries (TryGetPacked resolves
// a mesh only via the flat learned-DL index or the armed window), so the
// identity is recorded at two sound points instead: the render callback
// (every atomic render) and the TryGetPacked success path (exact allowlisted
// site draws). Map guarded by g_mutex; aggregates are atomics. The helper
// runs with g_mutex held by its caller (the callback hook takes it; the site
// hook is already inside TryGetPacked's lock_guard scope).
struct N8GeomKey {
    uint32_t countA = 0;
    uint32_t countB = 0;
    uint32_t meshCount = 0;
    bool operator<(const N8GeomKey& o) const {
        if (countA != o.countA) return countA < o.countA;
        if (countB != o.countB) return countB < o.countB;
        return meshCount < o.meshCount;
    }
};
struct N8GeomEntry {
    uint64_t renders = 0;
    uint64_t siteDraws = 0;
    bool allowlisted = false;
};
static std::map<N8GeomKey, N8GeomEntry> s_n8GeomMap;
static std::atomic<uint64_t> s_n8GeomRenders{0};
static std::atomic<uint64_t> s_n8GeomAllowRenders{0};
static std::atomic<uint64_t> s_n8GeomUncatRenders{0};
static std::atomic<uint64_t> s_n8GeomUnknownRenders{0};
static std::atomic<uint64_t> s_n8GeomSiteDraws{0};
// g_mutex held by the caller. isSiteDraw=false counts a render (and its
// allowlisted/uncatalogued split); isSiteDraw=true counts an exact
// allowlisted site draw. allowlisted is computed once, at first sighting:
// FindAllowlistRow(countA, countB) non-null AND row->meshCount == meshCount.
static void N8GeomNoteLocked(uint32_t countA, uint32_t countB, uint32_t meshCount, bool isSiteDraw) {
    const N8GeomKey key{countA, countB, meshCount};
    auto it = s_n8GeomMap.find(key);
    if (it == s_n8GeomMap.end()) {
        const AllowlistRow* row = FindAllowlistRow(countA, countB);
        const bool allowlisted = row != nullptr && row->meshCount == meshCount;
        it = s_n8GeomMap.emplace(key, N8GeomEntry{0, 0, allowlisted}).first;
    }
    if (isSiteDraw) {
        ++it->second.siteDraws;
        s_n8GeomSiteDraws.fetch_add(1, std::memory_order_relaxed);
    } else {
        ++it->second.renders;
        s_n8GeomRenders.fetch_add(1, std::memory_order_relaxed);
        if (it->second.allowlisted) {
            s_n8GeomAllowRenders.fetch_add(1, std::memory_order_relaxed);
        } else {
            s_n8GeomUncatRenders.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void PrintCounters(const char* why);

void CountFallback(Fallback f) {
    // Print on the first occurrence of each kind: a boot where the pin never
    // fully passes must not be silent (the 812f3f9e parity boot was — counted
    // fallbacks printed nothing because PrintCounters only fired on success).
    if (g_fallbacks[f].fetch_add(1, std::memory_order_relaxed) == 0) {
        PrintCounters("first fallback");
    }
}

// N3a: mirror CountFallback's first-occurrence print for the digest counters.
void CountDigestRetained() {
    if (g_digestRetained.fetch_add(1, std::memory_order_relaxed) == 0) {
        PrintCounters("first digest retained");
    }
}

void CountDigestChanged() {
    if (g_digestChanged.fetch_add(1, std::memory_order_relaxed) == 0) {
        PrintCounters("first digest changed");
    }
}

void CountDigestUnavailable() {
    if (g_digestUnavailable.fetch_add(1, std::memory_order_relaxed) == 0) {
        PrintCounters("first digest unavailable");
    }
}

// N3b: mirror the digest counters' first-occurrence print for the
// staging-skip counter.
void CountStagingSkipped() {
    if (g_stagingSkipped.fetch_add(1, std::memory_order_relaxed) == 0) {
        PrintCounters("first staging skipped");
    }
}

// N9 per-call timing counters (N4 diagnosis §3.2, N4-PGO build): total ns +
// call count for each nativescene entry point. The digest-resolve path is
// timed separately inside TryGetPacked/IsLearned; GenerationForRange is
// timed as called from this TU only (the DL scan probe's call in gx_dl.cpp
// is not nativescene work and stays untimed). One steady_clock read + two
// atomic adds per call (~50-100 ns); estimated cost ~0.1-0.2 ms/frame at
// 409 DLs/frame, to be confirmed by the flight from these counters.
// Printed by PrintCounters (periodic every 6000 arms, N6); there is no separate
// final-print site in this TU, so the last periodic dump is the final
// record. The ConvertDl pair is NOT defined here: it needs external
// linkage (gx_dl.cpp's No100fNativeSceneTryConvertDl timer references it
// through the header's extern declarations), so it is defined at file
// scope below the anonymous namespace, next to the bbox count functions.
std::atomic<uint64_t> g_n9IsLearnedNs{0};
std::atomic<uint64_t> g_n9IsLearnedCalls{0};
std::atomic<uint64_t> g_n9TryGetPackedNs{0};
std::atomic<uint64_t> g_n9TryGetPackedCalls{0};
std::atomic<uint64_t> g_n9DigestResolveNs{0};
std::atomic<uint64_t> g_n9DigestResolveCalls{0};
std::atomic<uint64_t> g_n9ArmBeginNs{0};
std::atomic<uint64_t> g_n9ArmBeginCalls{0};
std::atomic<uint64_t> g_n9PinScanCullNs{0};
std::atomic<uint64_t> g_n9PinScanCullCalls{0};
std::atomic<uint64_t> g_n9GenForRangeNs{0};
std::atomic<uint64_t> g_n9GenForRangeCalls{0};
// N5 ArmBegin sub-timers: split the admitted ArmBegin call into (a)
// pre-mutex pin work, (b) name walks, (c) EnsureCacheLocked / fold, (d)
// post-lock bookkeeping. The admitted remainder (~3-3.7 us/call) is
// unattributed by code read (N5 §3.1); these timers resolve it. Gated on
// the nativescene key like the other N9 timers; printed by PrintCounters.
// The sub-timers only fire on the admitted path (the early returns skip
// them), so the gap between arm_begin total and the sub-timer sums is the
// non-admitted fast path.
std::atomic<uint64_t> g_n9ArmPinNs{0};
std::atomic<uint64_t> g_n9ArmPinCalls{0};
std::atomic<uint64_t> g_n9ArmNameNs{0};
std::atomic<uint64_t> g_n9ArmNameCalls{0};
std::atomic<uint64_t> g_n9ArmEnsureNs{0};
std::atomic<uint64_t> g_n9ArmEnsureCalls{0};
std::atomic<uint64_t> g_n9ArmBookNs{0};
std::atomic<uint64_t> g_n9ArmBookCalls{0};
// N5: fingerprint re-learn firings (TryGetPacked step 2), cumulative; the
// flight diffs consecutive dumps for the per-window rate, which sizes
// flat-index maintenance.
std::atomic<uint32_t> g_n9Relearns{0};
// N6: ArmBegin lock + tail sub-timers (N6 §4.2.4b). Split the 8,182 ns/arm
// unattributed gap: lock acquisition time, and the post-bookkeeping tail
// (assignments + g_armCalls.fetch_add + %6000 + PrintCounters check).
// Gated on the n9 dk_diag key like the other N9 timers; printed on the
// n9sub line (only when n9 is on).
std::atomic<uint64_t> g_n9ArmLockNs{0};
std::atomic<uint64_t> g_n9ArmLockCalls{0};
std::atomic<uint64_t> g_n9ArmTailNs{0};
std::atomic<uint64_t> g_n9ArmTailCalls{0};
// N6: per-boot arm MISMATCH count (ArmEnd), printed on the counters line.
// The per-arm MISMATCH fprintf now fires at most once per geometry per
// boot (first occurrence); this counter carries the volume. The CHECK is
// unchanged (delta vs valid meshes still computed every arm; skull_key's
// double-submit shape stays counted, not printed per arm).
std::atomic<uint32_t> g_armMismatch{0};
// N6: geometries whose MISMATCH arm_done line has already printed once
// this boot. Guarded by g_mutex; only touched on the mismatch path in
// ArmEnd (g_mutex is free there — ArmBegin's lock is released before
// ArmEnd runs).
std::unordered_set<uint32_t> g_mismatchPrintedGeoms;
// N5: mirrors g_dlIndex.size() for PrintCounters. Declared here (with the
// other N9 counters) because PrintCounters is defined before g_caches and
// the index itself; the map and its helpers live below, next to g_caches.
std::atomic<uint64_t> g_dlIndexSizeSnap{0};
// N9 bbox falsifier (N4 diagnosis §4 candidate 7): GXClearBoundingBox
// calls and [gxbbox] reads per boot. Incremented only from the GX bbox
// code via No100fNativeSceneCountBboxClear/Read; both are inert when the
// nativescene key is off.
std::atomic<uint32_t> g_n9BboxClear{0};
std::atomic<uint32_t> g_n9BboxRead{0};

// N9: GenerationForRange as called from the nativescene path, timed.
// Replaces the direct calls in this TU (FoldGeneration's fold lambda,
// IsLearned step 4, TryGetPacked steps 2/3/4). N6: the timing is n9-gated
// like the scope timers — when the key is off this is a plain call.
uint64_t N9GenerationForRange(uint32_t addr, uint32_t nbytes) {
    if (!No100fN9Enabled()) return GxGuestWrite::GenerationForRange(addr, nbytes);
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t v = GxGuestWrite::GenerationForRange(addr, nbytes);
    g_n9GenForRangeNs.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count()),
        std::memory_order_relaxed);
    g_n9GenForRangeCalls.fetch_add(1, std::memory_order_relaxed);
    return v;
}

void PrintCounters(const char* why) {
    // N6: steady_clock calibration (§4.2.4c). At boot (first PrintCounters
    // call), run a tight loop of 10,000 steady_clock::now() calls and print
    // the mean ns/read once. One-time cost ~1-2 ms at boot; it calibrates
    // R directly and validates the N4 comment's ~50-100 ns estimate and
    // the §2.2f bound (R ≲ 50 ns from GenForRange).
    static bool clockCalPrinted = false;
    if (!clockCalPrinted) {
        clockCalPrinted = true;
        const auto calT0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 10000; ++i) {
            (void)std::chrono::steady_clock::now();
        }
        const auto calT1 = std::chrono::steady_clock::now();
        const uint64_t calMeanNs =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                calT1 - calT0).count()) / 10000;
        std::fprintf(stderr, "[nativescene] clock_cal mean_ns_per_read=%llu (n=10000)\n",
                     static_cast<unsigned long long>(calMeanNs));
        std::fflush(stderr);
    }
    // N6: the counters line always prints (small, needed for retention
    // gating); the n9 / n9sub / n9site telemetry lines print only when
    // the n9 dk_diag key is on. Periodic cadence is every 6000 arms
    // (was 600); first-occurrence prints are unchanged (rare).
    std::fprintf(stderr,
                 "[nativescene] counters (%s): native_draws=%u cache_builds=%u arms=%u "
                 "fallbacks pin_structure=%u name_mismatch=%u parse_invalid=%u gen_changed=%u "
                 "dl_unlearned=%u dl_drift=%u interp_on=%u ambiguous=%u window_extra_dl=%u "
                 "mesh_parse_invalid=%u learn_verts_mismatch=%u "
                 "digest_retained=%u digest_changed=%u digest_unavailable=%u "
                 "staging_skipped=%u arm_mismatch=%u\n",
                 why, g_nativeDraws.load(), g_cacheBuilds.load(), g_armCalls.load(),
                 g_fallbacks[kFbPinStructure].load(), g_fallbacks[kFbNameMismatch].load(),
                 g_fallbacks[kFbParseInvalid].load(), g_fallbacks[kFbGenChanged].load(),
                 g_fallbacks[kFbDlUnlearned].load(), g_fallbacks[kFbDlDrift].load(),
                 g_fallbacks[kFbInterpOn].load(), g_fallbacks[kFbAmbiguous].load(),
                 g_windowExtraDl.load(), g_fallbacks[kFbMeshParseInvalid].load(),
                 g_fallbacks[kFbLearnVertsMismatch].load(),
                 g_digestRetained.load(), g_digestChanged.load(), g_digestUnavailable.load(),
                 g_stagingSkipped.load(), g_armMismatch.load());
    std::fflush(stderr);
    if (No100fN9Enabled()) {
        // N5: the Aurora bbox walk stats (already instrumented in Aurora;
        // aurora_bbox_totals + AuroraStageTimerTotals, aurora/gfx.h).
        // Cumulative since start; the flight diffs consecutive dumps for
        // the per-window rates, like the other N9 counters. Plain atomic
        // loads, no locking (PrintCounters is called with g_mutex held
        // from the learn paths).
        uint64_t bboxVerts = 0, bboxAccumCalls = 0, bboxDisarms = 0;
        aurora_bbox_totals(&bboxVerts, &bboxAccumCalls, &bboxDisarms);
        uint64_t stageAdmit = 0, stageResolve = 0, stageUploads = 0, stageUniform = 0,
                 stageRecord = 0, stageBbox = 0;
        AuroraStageTimerTotals(&stageAdmit, &stageResolve, &stageUploads, &stageUniform,
                               &stageRecord, &stageBbox);
        std::fprintf(stderr,
                     "n9 is_learned_ns=%llu/%llu try_get_packed_ns=%llu/%llu "
                     "digest_resolve_ns=%llu/%llu convert_dl_ns=%llu/%llu "
                     "arm_begin_ns=%llu/%llu pin_scan_cull_ns=%llu/%llu "
                     "gen_for_range_ns=%llu/%llu bbox_clear=%u bbox_read=%u\n"
                     "n9sub arm_pin_ns=%llu/%llu arm_name_ns=%llu/%llu arm_ensure_ns=%llu/%llu "
                     "arm_book_ns=%llu/%llu arm_lock_ns=%llu/%llu arm_tail_ns=%llu/%llu "
                     "relearn=%u dl_index=%llu "
                     "bbox_verts=%llu bbox_accum_calls=%llu bbox_disarms=%llu bbox_walk_ns=%llu\n"
                     "n9site pre_ns=%llu/%llu post_ns=%llu/%llu\n",
                     static_cast<unsigned long long>(g_n9IsLearnedNs.load()),
                     static_cast<unsigned long long>(g_n9IsLearnedCalls.load()),
                     static_cast<unsigned long long>(g_n9TryGetPackedNs.load()),
                     static_cast<unsigned long long>(g_n9TryGetPackedCalls.load()),
                     static_cast<unsigned long long>(g_n9DigestResolveNs.load()),
                     static_cast<unsigned long long>(g_n9DigestResolveCalls.load()),
                     static_cast<unsigned long long>(g_n9TryConvertDlNs.load()),
                     static_cast<unsigned long long>(g_n9TryConvertDlCalls.load()),
                     static_cast<unsigned long long>(g_n9ArmBeginNs.load()),
                     static_cast<unsigned long long>(g_n9ArmBeginCalls.load()),
                     static_cast<unsigned long long>(g_n9PinScanCullNs.load()),
                     static_cast<unsigned long long>(g_n9PinScanCullCalls.load()),
                     static_cast<unsigned long long>(g_n9GenForRangeNs.load()),
                     static_cast<unsigned long long>(g_n9GenForRangeCalls.load()),
                     g_n9BboxClear.load(), g_n9BboxRead.load(),
                     static_cast<unsigned long long>(g_n9ArmPinNs.load()),
                     static_cast<unsigned long long>(g_n9ArmPinCalls.load()),
                     static_cast<unsigned long long>(g_n9ArmNameNs.load()),
                     static_cast<unsigned long long>(g_n9ArmNameCalls.load()),
                     static_cast<unsigned long long>(g_n9ArmEnsureNs.load()),
                     static_cast<unsigned long long>(g_n9ArmEnsureCalls.load()),
                     static_cast<unsigned long long>(g_n9ArmBookNs.load()),
                     static_cast<unsigned long long>(g_n9ArmBookCalls.load()),
                     static_cast<unsigned long long>(g_n9ArmLockNs.load()),
                     static_cast<unsigned long long>(g_n9ArmLockCalls.load()),
                     static_cast<unsigned long long>(g_n9ArmTailNs.load()),
                     static_cast<unsigned long long>(g_n9ArmTailCalls.load()),
                     g_n9Relearns.load(),
                     static_cast<unsigned long long>(g_dlIndexSizeSnap.load()),
                     static_cast<unsigned long long>(bboxVerts),
                     static_cast<unsigned long long>(bboxAccumCalls),
                     static_cast<unsigned long long>(bboxDisarms),
                     static_cast<unsigned long long>(stageBbox),
                     static_cast<unsigned long long>(g_n9SitePreNs.load()),
                     static_cast<unsigned long long>(g_n9SitePreCalls.load()),
                     static_cast<unsigned long long>(g_n9SitePostNs.load()),
                     static_cast<unsigned long long>(g_n9SitePostCalls.load()));
        std::fflush(stderr);
    }
}

// ---------------------------------------------------------------------------
// Guest reads (all big-endian-correct via the runtime's accessors).

uint16_t ReadGuest16(uint32_t addr) {
    return static_cast<uint16_t>((static_cast<uint32_t>(Memory::Read8(addr)) << 8) |
                                 static_cast<uint32_t>(Memory::Read8(addr + 1)));
}

bool ReadGuestName(uint32_t addr, std::string& out) {
    out.clear();
    for (int i = 0; i < 31; ++i) {
        const uint8_t byte = Memory::Read8(addr + static_cast<uint32_t>(i));
        if (byte == 0) break;
        if (byte < 0x20 || byte > 0x7E) return false;
        out.push_back(static_cast<char>(byte));
    }
    return !out.empty();
}

// Walks geometry -> mesh header -> mesh[i] material -> texture and reads
// the texture name (the chain the parse validates; shipping backdrop code
// proves [matList][0] -> [material+0] -> name at +0x10). Mesh entries are
// 12 bytes {indexData, indexCount, material} at meshBase + i*12 (N3 §3.2
// item 3); the single-mesh reader is index 0 of the same walk.
bool ReadTextureNameViaMeshIndex(uint32_t geom, uint32_t meshIndex, std::string& out) {
    if (!Memory::Contains(geom, 0x70)) return false;
    const uint32_t meshHeader = Memory::Read32(geom + 0x60);
    if (!meshHeader || !Memory::Contains(meshHeader, 16)) return false;
    const uint32_t meshBase = meshHeader + 16 + Memory::Read32(meshHeader + 0xC);
    const uint32_t entry = meshBase + meshIndex * 12;
    if (!Memory::Contains(entry, 12)) return false;
    const uint32_t material = Memory::Read32(entry + 8);
    if (!material || !Memory::Contains(material, 4)) return false;
    const uint32_t texture = Memory::Read32(material);
    if (!texture || !Memory::Contains(texture, 0x20)) return false;
    return ReadGuestName(texture + 0x10, out);
}

bool ReadTextureNameViaMesh(uint32_t geom, std::string& out) {
    return ReadTextureNameViaMeshIndex(geom, 0, out);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Cache. Stage 2 binds identity per geometry pointer: every distinct
// geometry passing the full pin gets its own entry, learned DL and arm
// accounting (rma00001 legitimately exists as two heap instances).

// Learned display-list identity for one mesh (STAGE2-DESK Q3, N3): (pointer,
// size) plus a guest-write generation fold over the DL range as a content
// guard, plus the DL's vertex span (maxIdx+1 from the player's scan) as the
// re-learn fingerprint. Untracked ranges fall back to (pointer, size)
// alone — the Stage 1 behaviour, and the documented residual risk. N3 Q2:
// one DL per mesh, so each mesh carries its own identity; a DL that arrives
// at a new address with the same (bytes, verts) fingerprint is the same
// mesh's list re-allocated, and re-learns onto that mesh.
struct DlIdentity {
    bool learned = false;
    uint32_t addr = 0;
    uint32_t bytes = 0;
    uint32_t verts = 0;  // DL vertex span (player scan maxIdx+1) at learn time
    uint64_t generation = 0;
    bool generationTracked = false;
    // N3a: XXH3_64bits content digest of the DL bytes at learn time (the same
    // digest the player's DL scan cache computes). When the generation fold
    // moves, the drift decision compares digests: identical bytes retain the
    // learn, only a genuine content change drifts to GX. digestValid is false
    // when no digest could be taken at learn time; the mesh then behaves
    // exactly as it did before N3a.
    uint64_t digest = 0;
    bool digestValid = false;
};

// One mesh of a parsed geometry (N3 §3.2 item 2). All meshes index the same
// shared vertex arrays; each mesh has its own index array, material, packed
// stream, and learned DL. A mesh that fails validation is marked invalid
// and stays GX; it never poisons its siblings.
struct MeshCache {
    bool valid = false;
    std::vector<uint8_t> packed;
    uint16_t vertexCount = 0;  // == indexCount (strip length)
    uint32_t indexData = 0;
    uint32_t indexCount = 0;
    uint32_t maxIndex = 0;  // max vertex index referenced (learn cross-check)
    std::string textureName;
    uint32_t material = 0;
    DlIdentity dl;
};

struct Cache {
    bool valid = false;
    bool poisoned = false;
    uint32_t geometry = 0;
    uint16_t serial = 0;
    uint64_t generation = 0;
    bool generationTracked = false;
    std::string textureName;  // mesh[0]'s texture (the pin name)
    // Fold inputs, kept so the generation can be re-sampled verbatim.
    uint32_t meshHeader = 0, meshBase = 0, positions = 0, normals = 0, colours = 0, uv = 0;
    uint32_t numVertices = 0;
    uint32_t meshCount = 0;
    uint32_t arms = 0;  // arms served (per-arm draw-count log cadence)
    std::vector<MeshCache> meshes;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Cache> g_caches;

// N5 flat learned-DL index: O(1) lookup for TryGetPacked step 1, replacing
// the linear scan over all of g_caches x meshes (~95 heap-scattered entries,
// 1,002 ns/call on the N4 flight). Keyed by ((uint64_t)addr << 32 | bytes),
// value (geometry, meshIndex). Maintained EAGERLY at the three mutation
// sites (first learn, fingerprint re-learn, drift-unlearn) and cleared for
// a geometry whenever its cache is invalidated or poisoned. Guarded by
// g_mutex like g_caches; every access site holds it. Lookup validates the
// same conditions as the old scan (mesh valid, dl.learned, dl.addr == addr,
// dl.bytes == bytes, ownerCache valid); a stale entry can only cause a miss
// + one wasted validation, never a wrong convert, because the (addr, bytes)
// key is re-checked against the stored mesh. Two meshes sharing one
// (addr, bytes) key (same DL size landing at the same scratch address) keep
// the last-learned entry — the same arbitration the old scan did by map
// order. g_dlIndexSizeSnap mirrors the size for PrintCounters, which must
// not lock g_mutex (it is called with the mutex held from the learn paths);
// it is declared above with the N9 counters because PrintCounters is
// defined before this point.
std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> g_dlIndex;

inline uint64_t DlIndexKey(uint32_t addr, uint32_t nbytes) {
    return (static_cast<uint64_t>(addr) << 32) | nbytes;
}

// g_mutex held by the caller.
void DlIndexAdd(uint32_t geom, uint32_t meshIndex, uint32_t addr, uint32_t nbytes) {
    g_dlIndex[DlIndexKey(addr, nbytes)] = std::make_pair(geom, meshIndex);
    g_dlIndexSizeSnap.store(static_cast<uint64_t>(g_dlIndex.size()), std::memory_order_relaxed);
}

// Remove only if the key still maps to this mesh (a newer learn may have
// taken the key for another mesh). g_mutex held by the caller.
void DlIndexRemove(uint32_t geom, uint32_t meshIndex, uint32_t addr, uint32_t nbytes) {
    const auto it = g_dlIndex.find(DlIndexKey(addr, nbytes));
    if (it != g_dlIndex.end() && it->second.first == geom && it->second.second == meshIndex) {
        g_dlIndex.erase(it);
        g_dlIndexSizeSnap.store(static_cast<uint64_t>(g_dlIndex.size()), std::memory_order_relaxed);
    }
}

// Drop every entry owned by one geometry (cache invalidated / re-parsed /
// poisoned: its meshes hold no learned DLs anymore). g_mutex held by the caller.
void ClearDlIndexForGeometry(uint32_t geom) {
    bool changed = false;
    for (auto it = g_dlIndex.begin(); it != g_dlIndex.end();) {
        if (it->second.first == geom) {
            it = g_dlIndex.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        g_dlIndexSizeSnap.store(static_cast<uint64_t>(g_dlIndex.size()), std::memory_order_relaxed);
    }
}
std::vector<uint32_t> g_boundGeometries;    // first-binding audit log (per pointer)
std::vector<uint32_t> g_engagedGeometries;  // engaged line printed once per geometry
std::vector<std::pair<uint32_t, uint32_t>> g_extraDlSeen;  // first-sighting lines, capped at 16
bool g_censusOverflowLogged = false;

uint64_t FoldGeneration(Cache& c) {
    using GxGuestWrite::kUntracked;
    uint64_t folded = 0;
    bool tracked = true;
    const auto add = [&](uint32_t addr, uint32_t nbytes) {
        const uint64_t v = N9GenerationForRange(addr, nbytes);
        if (v == kUntracked) {
            tracked = false;
        } else {
            folded += v;
        }
    };
    add(c.geometry, 0x70);
    add(c.meshHeader, 16);
    add(c.meshBase, 12 * c.meshCount);
    add(c.positions, c.numVertices * 12);
    add(c.normals, c.numVertices * 12);
    add(c.colours, c.numVertices * 4);
    add(c.uv, c.numVertices * 8);
    for (const auto& m : c.meshes) add(m.indexData, m.indexCount * 2);
    c.generationTracked = tracked;
    return folded;
}

void AppendBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void AppendBEFloat(std::vector<uint8_t>& out, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    AppendBE32(out, bits);
}

// Parses and validates the geometry into out. On failure returns false and
// sets *reason; the caller poisons the geometry. A per-mesh failure does
// NOT fail the geometry: the mesh is marked invalid (logged with its index)
// and stays GX while its siblings still convert; only zero valid meshes
// fails the geometry. g_mutex held by the caller.
bool ParseGeometry(uint32_t atomic, uint32_t geom, const AllowlistRow* row, Cache& out,
                   const char** reason) {
    Cache next;
    next.geometry = geom;
    const auto fail = [&](const char* r) {
        *reason = r;
        return false;
    };

    if (!Memory::Contains(geom, 0x70)) return fail("geometry_extent");
    const uint32_t numVertices = Memory::Read32(geom + 0x14);
    if (numVertices == 0 || numVertices > 65535) return fail("vertex_count");
    next.numVertices = numVertices;

    const uint32_t morph = Memory::Read32(geom + 0x68);
    if (!morph || !Memory::Contains(morph, 0x1C)) return fail("morph_ptr");
    next.positions = Memory::Read32(morph + 0x14);
    next.normals = Memory::Read32(morph + 0x18);
    next.colours = Memory::Read32(geom + 0x30);
    next.uv = Memory::Read32(geom + 0x34);
    if (!next.positions || !Memory::Contains(next.positions, numVertices * 12)) return fail("positions_extent");
    if (!next.normals || !Memory::Contains(next.normals, numVertices * 12)) return fail("normals_extent");
    if (!next.colours || !Memory::Contains(next.colours, numVertices * 4)) return fail("colours_extent");
    if (!next.uv || !Memory::Contains(next.uv, numVertices * 8)) return fail("uv_extent");

    const uint32_t meshHeader = Memory::Read32(geom + 0x60);
    if (!meshHeader || !Memory::Contains(meshHeader, 16)) return fail("mesh_header");
    next.meshHeader = meshHeader;
    next.serial = ReadGuest16(meshHeader + 6);
    const uint32_t meshCount = ReadGuest16(meshHeader + 4);
    if (meshCount == 0 || meshCount > kMaxMeshes) return fail("mesh_count");
    if (row != nullptr && meshCount != row->meshCount) return fail("mesh_count");
    next.meshCount = meshCount;
    next.meshBase = meshHeader + 16 + Memory::Read32(meshHeader + 0xC);
    if (!Memory::Contains(next.meshBase, 12 * meshCount)) return fail("mesh_extent");

    // Validation battery (shared arrays): the parse is untrusted until all
    // of this passes. Runs once per geometry; per-mesh index checks follow.
    const float centreX = Memory::ReadFloat32(atomic + 0x1C);
    const float centreY = Memory::ReadFloat32(atomic + 0x20);
    const float centreZ = Memory::ReadFloat32(atomic + 0x24);
    const float radius = Memory::ReadFloat32(atomic + 0x28);
    const float maxDist2 = (3.0f * radius) * (3.0f * radius);
    std::vector<float> pos(numVertices * 3);
    std::vector<float> nrm(numVertices * 3);
    uint32_t alphaOk = 0;
    for (uint32_t i = 0; i < numVertices; ++i) {
        for (int k = 0; k < 3; ++k) {
            pos[i * 3 + k] = Memory::ReadFloat32(next.positions + i * 12 + k * 4);
            nrm[i * 3 + k] = Memory::ReadFloat32(next.normals + i * 12 + k * 4);
            if (!std::isfinite(pos[i * 3 + k]) || !std::isfinite(nrm[i * 3 + k])) return fail("non_finite");
        }
        const float dx = pos[i * 3] - centreX, dy = pos[i * 3 + 1] - centreY, dz = pos[i * 3 + 2] - centreZ;
        if (dx * dx + dy * dy + dz * dz > maxDist2) return fail("position_outside_sphere");
        const float u = Memory::ReadFloat32(next.uv + i * 8);
        const float v = Memory::ReadFloat32(next.uv + i * 8 + 4);
        if (!std::isfinite(u) || !std::isfinite(v) || u < -8.0f || u > 8.0f || v < -8.0f || v > 8.0f) {
            return fail("uv_range");
        }
        if (Memory::Read8(next.colours + i * 4 + 3) == 0xFF) ++alphaOk;
    }
    uint32_t normalOk = 0;
    for (uint32_t i = 0; i < numVertices; ++i) {
        const float len = std::sqrt(nrm[i * 3] * nrm[i * 3] + nrm[i * 3 + 1] * nrm[i * 3 + 1] +
                                    nrm[i * 3 + 2] * nrm[i * 3 + 2]);
        if (std::fabs(len - 1.0f) <= 0.1f) ++normalOk;
    }
    if (normalOk * 100 < numVertices * 99) return fail("normals_not_unit");
    if (alphaOk * 100 < numVertices * 99) return fail("colour_alpha");

    // Per-mesh parse + validation (N3 §3.2 items 1, 3, 5). Each mesh's index
    // array is validated independently; the union of all meshes' max indices
    // must satisfy the histogram (a mesh legitimately uses a vertex subset,
    // so the per-mesh histogram is not required). A mesh failure poisons
    // that mesh only.
    next.meshes.resize(meshCount);
    uint32_t validMeshes = 0;
    uint32_t unionMaxIndex = 0;
    uint32_t raster0 = 0, cpPixels0 = 0, texW0 = 0, texH0 = 0;
    std::vector<uint16_t> firstValidIndices;
    for (uint32_t mi = 0; mi < meshCount; ++mi) {
        MeshCache& m = next.meshes[mi];
        const auto failMesh = [&](const char* r) {
            CountFallback(kFbMeshParseInvalid);
            std::fprintf(stderr, "[nativescene] mesh_parse_invalid geom=%08X mesh=%u reason=%s tex=\"%s\"\n",
                         geom, mi, r, m.textureName.c_str());
            std::fflush(stderr);
        };
        const uint32_t entry = next.meshBase + mi * 12;
        m.indexData = Memory::Read32(entry);
        m.indexCount = Memory::Read32(entry + 4);
        m.material = Memory::Read32(entry + 8);
        if (m.indexCount < 3 || m.indexCount > 65535) { failMesh("index_count"); continue; }
        if (!m.indexData || !Memory::Contains(m.indexData, m.indexCount * 2)) { failMesh("index_extent"); continue; }
        if (!m.material || !Memory::Contains(m.material, 4)) { failMesh("material_ptr"); continue; }
        const uint32_t texture = Memory::Read32(m.material);
        if (!texture || !Memory::Contains(texture, 0x20)) { failMesh("texture_ptr"); continue; }
        if (!ReadGuestName(texture + 0x10, m.textureName)) { failMesh("texture_name"); continue; }
        if (row != nullptr && mi < row->nameCount && row->names[mi] != nullptr &&
            m.textureName != row->names[mi]) {
            failMesh("name_mismatch");
            continue;
        }
        if (mi == 0) {
            if (!Memory::Contains(texture, 0x54)) { failMesh("texture_ptr"); continue; }
            const uint32_t raster = Memory::Read32(texture);
            if (!raster || !Memory::Contains(raster, 0x14)) { failMesh("raster_ptr"); continue; }
            raster0 = raster;
            cpPixels0 = Memory::Read32(raster + 4);
            texW0 = Memory::Read32(raster + 0x0C);
            texH0 = Memory::Read32(raster + 0x10);
        }
        std::vector<uint16_t> indices(m.indexCount);
        bool indexOk = true;
        for (uint32_t i = 0; i < m.indexCount; ++i) {
            indices[i] = ReadGuest16(m.indexData + i * 2);
            if (indices[i] >= numVertices) { failMesh("index_out_of_range"); indexOk = false; break; }
            if (indices[i] > m.maxIndex) m.maxIndex = indices[i];
        }
        if (!indexOk) continue;
        if (m.maxIndex > unionMaxIndex) unionMaxIndex = m.maxIndex;
        // Pack the strip-expanded direct-vertex stream (big-endian GX
        // format, the same byte order submit_raw_draw's other caller packs).
        m.packed.reserve(static_cast<size_t>(m.indexCount) * kVertexBytes);
        for (uint32_t s = 0; s < m.indexCount; ++s) {
            const uint32_t i = indices[s];
            AppendBEFloat(m.packed, pos[i * 3]);
            AppendBEFloat(m.packed, pos[i * 3 + 1]);
            AppendBEFloat(m.packed, pos[i * 3 + 2]);
            AppendBEFloat(m.packed, nrm[i * 3]);
            AppendBEFloat(m.packed, nrm[i * 3 + 1]);
            AppendBEFloat(m.packed, nrm[i * 3 + 2]);
            for (int b = 0; b < 4; ++b) m.packed.push_back(Memory::Read8(next.colours + i * 4 + b));
            AppendBEFloat(m.packed, Memory::ReadFloat32(next.uv + i * 8));
            AppendBEFloat(m.packed, Memory::ReadFloat32(next.uv + i * 8 + 4));
        }
        m.vertexCount = static_cast<uint16_t>(m.indexCount);
        m.valid = true;
        ++validMeshes;
        if (firstValidIndices.empty()) firstValidIndices = std::move(indices);
    }
    if (validMeshes == 0) return fail("all_meshes_invalid");
    if (numVertices >= 3 && unionMaxIndex < numVertices - 2) return fail("index_histogram");
    if (next.meshes[0].valid) {
        next.textureName = next.meshes[0].textureName;
    } else if (!ReadTextureNameViaMesh(geom, next.textureName)) {
        next.textureName.clear();
    }
    next.generation = FoldGeneration(next);
    next.valid = true;

    if (std::find(g_engagedGeometries.begin(), g_engagedGeometries.end(), geom) ==
        g_engagedGeometries.end()) {
        g_engagedGeometries.push_back(geom);
        std::fprintf(stderr,
                     "[nativescene] engaged geom=%08X atomic=%08X counts=%u/%u morph=%u mats=%u "
                     "meshes=%u valid_meshes=%u serial=%u tex=\"%s\" raster=%08X cpPixels=%08X %ux%u pos=%08X nrm=%08X "
                     "clr=%08X uv=%08X idx=%08X nidx=%u expected_addr_match=%d\n",
                     geom, atomic, Memory::Read32(geom + 0x10), numVertices,
                     Memory::Read32(geom + 0x18), Memory::Read32(geom + 0x24), meshCount,
                     validMeshes, next.serial, next.textureName.c_str(), raster0, cpPixels0, texW0, texH0,
                     next.positions, next.normals, next.colours, next.uv,
                     next.meshes[0].indexData, next.meshes[0].indexCount,
                     geom == kExpectedGeometry ? 1 : 0);
        std::string names;
        for (uint32_t mi = 0; mi < meshCount; ++mi) {
            if (mi != 0) names += ",";
            names += next.meshes[mi].valid ? next.meshes[mi].textureName : "(invalid)";
        }
        std::fprintf(stderr, "[nativescene] meshes geom=%08X count=%u names=\"%s\"\n", geom,
                     meshCount, names.c_str());
        if (!firstValidIndices.empty()) {
            std::fprintf(stderr,
                         "[nativescene] first tri idx=%u,%u,%u v0 pos=(%.3f,%.3f,%.3f) nrm=(%.3f,%.3f,%.3f) "
                         "uv=(%.3f,%.3f)\n",
                         firstValidIndices[0], firstValidIndices[1], firstValidIndices[2], pos[0], pos[1], pos[2], nrm[0], nrm[1],
                         nrm[2], Memory::ReadFloat32(next.uv), Memory::ReadFloat32(next.uv + 4));
        }
        std::fflush(stderr);
    }

    out = std::move(next);
    return true;
}

// g_mutex held by the caller.
bool EnsureCacheLocked(uint32_t atomic, uint32_t geom, const AllowlistRow* row) {
    const auto existing = g_caches.find(geom);
    if (existing != g_caches.end() && existing->second.poisoned) return false;
    if (existing != g_caches.end() && existing->second.valid) {
        Cache& cached = existing->second;
        const uint32_t meshHeader = Memory::Read32(geom + 0x60);
        const uint16_t serial = (meshHeader && Memory::Contains(meshHeader, 16))
                                    ? ReadGuest16(meshHeader + 6)
                                    : static_cast<uint16_t>(~cached.serial);
        bool fresh = serial == cached.serial;
        if (fresh && cached.generationTracked) {
            fresh = FoldGeneration(cached) == cached.generation;
        }
        if (fresh) return true;
        cached.valid = false;
        // N5: the cache's meshes hold no learned DLs anymore — drop its
        // flat-index entries (the next lookup would fail validation and
        // erase them anyway; eager clearing keeps the index exact).
        ClearDlIndexForGeometry(geom);
        CountFallback(kFbGenChanged);
    }
    const char* reason = nullptr;
    Cache next;
    if (ParseGeometry(atomic, geom, row, next, &reason)) {
        g_cacheBuilds.fetch_add(1, std::memory_order_relaxed);
        // N5: the fresh cache holds no learned DLs; drop the old entries.
        ClearDlIndexForGeometry(geom);
        g_caches[geom] = std::move(next);
        return true;
    }
    Cache poison;
    poison.poisoned = true;
    poison.geometry = geom;
    // N5: poisoned geometries hold no learned DLs; drop the entries.
    ClearDlIndexForGeometry(geom);
    g_caches[geom] = std::move(poison);
    CountFallback(kFbParseInvalid);
    std::fprintf(stderr, "[nativescene] parse_invalid geom=%08X reason=%s\n", geom,
                 reason ? reason : "?");
    std::fprintf(stderr, "[nativescene] geom dump:");
    for (uint32_t off = 0; off < 0x84; off += 4) {
        std::fprintf(stderr, " %08X", Memory::Contains(geom + off, 4) ? Memory::Read32(geom + off) : 0);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    return false;
}


// Arm record: written and read on the game thread only (the render callback
// and the guest display-list call both execute there).
struct ArmState {
    bool active = false;
    uint32_t atomic = 0;
    uint32_t geometry = 0;
    // Per-window DL census (Stage 2, STAGE2-DESK Q3): every distinct DL
    // noted inside this window. Reset at ArmBegin and kept after ArmEnd so
    // the conversion site can learn from it. The census never disables
    // anything; non-learned DLs pass through the player on GX untouched.
    // N3: 16 entries (was 8) — a 5-mesh geometry notes 5 DLs plus extras.
    struct CensusEntry {
        uint32_t addr = 0;
        uint32_t bytes = 0;
    };
    CensusEntry census[16];
    int censusCount = 0;
    bool censusOverflow = false;
    // N3 §3.4 draw-count check: native draws served during this arm, and
    // the valid-mesh count they are expected to equal.
    uint32_t armNativeDrawsStart = 0;
    uint32_t armMeshCount = 0;
    uint32_t armValidMeshes = 0;
    uint32_t armIndex = 0;  // this geometry's arm number (Cache.arms at arm time)
};
ArmState g_arm;

// ---------------------------------------------------------------------------
// Engagement diagnostics (Stage 1 diag build): the 812f3f9e parity boot
// produced zero [nativescene] lines, and the log could not distinguish gate
// off / callback never entered / pin never matched. These make each of those
// states print exactly once per distinct fact. All gated by the same key.

std::atomic<uint32_t> g_callbackEntries{0};
std::vector<uint32_t> g_scannedCb;
std::vector<uint32_t> g_scannedCull;

// Evaluates the ArmBegin pin conditions against one geometry and prints the
// verdict, naming the first failed condition with the actual field values
// (w1C/w28 included: on the title fixture +0x18/+0x1C/+0x24/+0x28 all read 1,
// so the scan must show which words the real geometries carry). Runs once
// per distinct geometry per source ("cb" = render callback, "cull" =
// iModelCull). Takes g_mutex for the seen set only.
void PinScan(uint32_t atomic, uint32_t geom, const char* source,
             std::vector<uint32_t>& seen) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (std::find(seen.begin(), seen.end(), geom) != seen.end()) return;
        if (seen.size() >= 512) return;
        seen.push_back(geom);
    }
    const uint32_t countA = Memory::Read32(geom + 0x10);
    const uint32_t countB = Memory::Read32(geom + 0x14);
    const uint32_t w18 = Memory::Read32(geom + 0x18);
    const uint32_t w1C = Memory::Read32(geom + 0x1C);
    const uint32_t w24 = Memory::Read32(geom + 0x24);
    const uint32_t w28 = Memory::Read32(geom + 0x28);
    uint32_t meshCount = 0xFFFFFFFFu;
    const uint32_t meshHeader = Memory::Read32(geom + 0x60);
    if (meshHeader && Memory::Contains(meshHeader, 16)) {
        meshCount = ReadGuest16(meshHeader + 4);
    }
    std::string texName;
    const bool nameOk = ReadTextureNameViaMesh(geom, texName);
    const AllowlistRow* row = FindAllowlistRow(countA, countB);
    const char* verdict = "PASS";
    if (row == nullptr) {
        verdict = "FAIL not_in_allowlist";
    } else if (w18 != 1) {
        verdict = "FAIL morph_w18";
    } else if (w24 != row->meshCount) {
        verdict = "FAIL materials_w24";
    } else if (meshCount != row->meshCount) {
        verdict = "FAIL mesh_count";
    } else if (nameOk && texName != row->names[0]) {
        verdict = "FAIL name_mismatch";
    } else if (!nameOk) {
        verdict = "FAIL name_read";
    }
    std::fprintf(stderr,
                 "[nativescene] pin-scan[%s] geom=%08X atomic=%08X counts=%u/%u w18=%u w1C=%u "
                 "w24=%u w28=%u meshes=%u tex=\"%s\" verdict=%s\n",
                 source, geom, atomic, countA, countB, w18, w1C, w24, w28, meshCount,
                 nameOk ? texName.c_str() : "?", verdict);
    std::fflush(stderr);
}

// First-20 callback entries: proves whether the 0x801488B8 override is
// entered at all, and what it sees. Called at the override's entry.
void DiagCallbackEntry(uint32_t atomic) {
    if (!No100fNativeSceneEnabled()) return;
    const uint32_t n = g_callbackEntries.fetch_add(1, std::memory_order_relaxed);
    uint32_t geom = 0, countA = 0, countB = 0;
    bool geomOk = false;
    if (atomic && Memory::Contains(atomic, 0x70)) {
        geom = Memory::Read32(atomic + 0x18);
        if (geom && Memory::Contains(geom, 0x70)) {
            geomOk = true;
            countA = Memory::Read32(geom + 0x10);
            countB = Memory::Read32(geom + 0x14);
        }
    }
    if (n < 20) {
        std::fprintf(stderr,
                     "[nativescene] cb entry #%u atomic=%08X geom=%08X counts=%u/%u\n",
                     n + 1, atomic, geom, countA, countB);
        std::fflush(stderr);
    }
    if (geom) PinScan(atomic, geom, "cb", g_scannedCb);
    // N8 C5: every atomic render, keyed by geometry identity. meshCount
    // mirrors PinScan (meshHeader +4, fallback to geom+0x24); a render whose
    // geometry is unreadable counts as unknown, not as an identity.
    if (geomOk) {
        uint32_t meshCount = 0xFFFFFFFFu;
        const uint32_t meshHeader = Memory::Read32(geom + 0x60);
        if (meshHeader && Memory::Contains(meshHeader, 16)) {
            meshCount = ReadGuest16(meshHeader + 4);
        }
        if (meshCount == 0xFFFFFFFFu) meshCount = Memory::Read32(geom + 0x24);
        std::lock_guard<std::mutex> lock(g_mutex);
        N8GeomNoteLocked(countA, countB, meshCount, /*isSiteDraw=*/false);
    } else {
        s_n8GeomUnknownRenders.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

// N9: the ConvertDl counter pair needs external linkage (gx_dl.cpp's
// No100fNativeSceneTryConvertDl timer references it through the header's
// extern declarations), so it is defined here at file scope, outside the
// anonymous namespace above. Likewise the bbox count functions are public
// (declared in the header, called from gx_stubs.cpp), so they live here
// too; they increment the anonymous-namespace bbox counters, which are
// visible at file scope after the namespace block. N6: the fold-site
// (n9site) pairs follow the same pattern (gx_dl.cpp's submission-site
// timers reference them through the header).
std::atomic<uint64_t> g_n9TryConvertDlNs{0};
std::atomic<uint64_t> g_n9TryConvertDlCalls{0};
std::atomic<uint64_t> g_n9SitePreNs{0};
std::atomic<uint64_t> g_n9SitePreCalls{0};
std::atomic<uint64_t> g_n9SitePostNs{0};
std::atomic<uint64_t> g_n9SitePostCalls{0};

// N6: the N9 timing gate (dk_diag key "n9" -> NO100F_N9, set by
// switch/deko/device.cpp's diag_init like the nativescene key). Read once
// at first use into a static bool. Deliberately silent: no gate-read
// print, so the toggle flight's exactly-one-[nativescene]-line check is
// preserved (the n9 state is observable from the n9 lines themselves).
bool No100fN9Enabled() {
    static const bool enabled = [] {
        const char* raw = std::getenv("NO100F_N9");
        return raw != nullptr;
    }();
    return enabled;
}

void No100fNativeSceneCountBboxClear() {
    if (!No100fNativeSceneEnabled()) return;
    g_n9BboxClear.fetch_add(1, std::memory_order_relaxed);
}

void No100fNativeSceneCountBboxRead() {
    if (!No100fNativeSceneEnabled()) return;
    g_n9BboxRead.fetch_add(1, std::memory_order_relaxed);
}

bool No100fNativeSceneEnabled() {
    static const bool enabled = [] {
        const char* raw = std::getenv("NO100F_NATIVESCENE");
        std::fprintf(stderr,
                     "[nativescene] gate read: NO100F_NATIVESCENE=%s -> %s "
                     "(frame_interpolation_fps=%u)\n",
                     raw ? raw : "(unset)", raw ? "ON" : "OFF",
                     RuntimeConfigFile::FrameInterpolationFps());
        std::fflush(stderr);
        return raw != nullptr;
    }();
    return enabled;
}

bool No100fNativeSceneArmBegin(uint32_t atomic) {
    if (!No100fNativeSceneEnabled()) return false;
    No100fN9ScopeTimer n9(g_n9ArmBeginNs, g_n9ArmBeginCalls);
    // N6: the manual pin/lock/tail timings below are n9-gated like the
    // scope timers (one flag read per ArmBegin call, reused for all three).
    const bool n9On = No100fN9Enabled();
    // N5 (a): pre-mutex pin work starts here. Manual timing: the early
    // returns below skip the add, so only admitted-path calls (those
    // reaching the mutex) are counted.
    const auto pinT0 = n9On ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
    if (g_arm.active) return false;  // no nesting; the execute call is not re-entrant here
    if (RuntimeConfigFile::FrameInterpolationFps() != 0) {
        CountFallback(kFbInterpOn);
        return false;
    }
    if (!atomic || !Memory::Contains(atomic, 0x70)) return false;
    const uint32_t geom = Memory::Read32(atomic + 0x18);
    if (!geom || !Memory::Contains(geom, 0x70)) return false;
    const AllowlistRow* row =
        FindAllowlistRow(Memory::Read32(geom + 0x10), Memory::Read32(geom + 0x14));
    if (row == nullptr) {
        return false;  // not admitted: silent, this is every other atomic
    }
    if (Memory::Read32(geom + 0x18) != 1 || Memory::Read32(geom + 0x24) != row->meshCount) {
        CountFallback(kFbPinStructure);
        return false;
    }
    // N5 (a): pin work ends at the mutex.
    if (n9On) {
        g_n9ArmPinNs.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - pinT0).count()),
            std::memory_order_relaxed);
        g_n9ArmPinCalls.fetch_add(1, std::memory_order_relaxed);
    }
    {
        // N6 (lock): time the mutex acquisition itself — from before the
        // lock_guard construction to after it returns.
        const auto lockT0 = n9On ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
        std::lock_guard<std::mutex> lock(g_mutex);
        if (n9On) {
            g_n9ArmLockNs.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - lockT0).count()),
                std::memory_order_relaxed);
            g_n9ArmLockCalls.fetch_add(1, std::memory_order_relaxed);
        }
        // Per-pointer identity (Stage 2): every distinct geometry passing
        // the full pin gets its own cache entry and learned DL. The first
        // binding per pointer is logged so bindings stay auditable; there
        // is no boot-fatal ambiguity state.
        if (std::find(g_boundGeometries.begin(), g_boundGeometries.end(), geom) ==
            g_boundGeometries.end()) {
            g_boundGeometries.push_back(geom);
            std::fprintf(stderr, "[nativescene] bound geom=%08X tex=\"%s\" (binding #%zu)\n",
                         geom, row->names[0], g_boundGeometries.size());
            std::fflush(stderr);
        }
        const auto boundIt = g_caches.find(geom);
        if (boundIt != g_caches.end() && boundIt->second.poisoned) return false;
        std::string pinName;
        bool pinNameOk = false;
        {
            // N5 (b): first name walk.
            No100fN9ScopeTimer nameT(g_n9ArmNameNs, g_n9ArmNameCalls);
            pinNameOk = ReadTextureNameViaMesh(geom, pinName);
        }
        if (!pinNameOk || pinName != row->names[0]) {
            CountFallback(kFbNameMismatch);
            std::fprintf(stderr,
                         "[nativescene] name_mismatch geom=%08X expected=\"%s\" actual=\"%s\"; "
                         "geometry poisoned for this boot (GX fallback)\n",
                         geom, row->names[0], pinName.empty() ? "?" : pinName.c_str());
            std::fflush(stderr);
            Cache poison;
            poison.poisoned = true;
            poison.geometry = geom;
            // N5: poisoned geometries hold no learned DLs; drop the entries.
            ClearDlIndexForGeometry(geom);
            g_caches[geom] = std::move(poison);
            return false;
        }
        // N5 (c): EnsureCacheLocked / fold.
        bool cacheOk = false;
        {
            No100fN9ScopeTimer ensureT(g_n9ArmEnsureNs, g_n9ArmEnsureCalls);
            cacheOk = EnsureCacheLocked(atomic, geom, row);
        }
        if (!cacheOk) return false;
        std::string nowName;
        const auto entryIt = g_caches.find(geom);
        // N5 (b): second name walk. Hoisted out of the condition to time
        // it, but the walk still only runs when the entry exists (the old
        // short-circuit is preserved exactly).
        bool nowNameOk = false;
        if (entryIt != g_caches.end()) {
            No100fN9ScopeTimer nameT(g_n9ArmNameNs, g_n9ArmNameCalls);
            nowNameOk = ReadTextureNameViaMesh(geom, nowName);
        }
        if (entryIt == g_caches.end() || !nowNameOk ||
            nowName != entryIt->second.textureName) {
            CountFallback(kFbNameMismatch);
            return false;
        }
        // N3 §3.4: per-arm draw-count bookkeeping — the valid-mesh count
        // this arm is expected to serve as native draws.
        // N5 (d): post-lock bookkeeping.
        {
            No100fN9ScopeTimer bookT(g_n9ArmBookNs, g_n9ArmBookCalls);
            if (entryIt != g_caches.end() && entryIt->second.valid) {
                g_arm.armMeshCount = entryIt->second.meshCount;
                uint32_t valid = 0;
                for (const auto& m : entryIt->second.meshes) {
                    if (m.valid) ++valid;
                }
                g_arm.armValidMeshes = valid;
                g_arm.armIndex = ++entryIt->second.arms;
            }
        }
    }
    // N6 (tail): post-bookkeeping assignments + g_armCalls.fetch_add +
    // %6000 + PrintCounters check (after the bookT scope closes and the
    // lock is released).
    const auto tailT0 = n9On ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    g_arm.censusCount = 0;
    g_arm.censusOverflow = false;
    g_arm.active = true;
    g_arm.atomic = atomic;
    g_arm.geometry = geom;
    g_arm.armNativeDrawsStart = g_nativeDraws.load(std::memory_order_relaxed);
    if (g_armCalls.fetch_add(1, std::memory_order_relaxed) % 6000 == 5999) {
        PrintCounters("periodic");
    }
    if (n9On) {
        g_n9ArmTailNs.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - tailT0).count()),
            std::memory_order_relaxed);
        g_n9ArmTailCalls.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void No100fNativeSceneArmEnd() {
    // N3 §3.4: per-arm draw-count check. The atomic's DLs play
    // synchronously inside RxPipelineExecute, so by ArmEnd the delta is the
    // number of native draws this arm served. Log on the first arm, on a
    // mismatch (delta != valid meshes), and every 600th arm; silence is the
    // pass case. (The "other shape" assertion — one DL per mesh, not one DL
    // for all meshes — rides on this check: a single-DL geometry would log
    // native_draws=1 against valid=5. The player exposes no per-DL draw
    // count to the conversion site, so there is no cheaper runtime assert.)
    // N6: the MISMATCH line now prints at most ONCE per geometry per boot
    // (first occurrence); every mismatch is still COUNTED
    // (g_armMismatch, printed on the counters line). The CHECK is
    // unchanged — delta vs valid meshes is still computed every arm, so
    // skull_key's double-submit shape stays counted, not printed per arm.
    // The every-600th-arm line (the §3.4 sampling) is unchanged.
    if (g_arm.active) {
        const uint32_t delta = g_nativeDraws.load(std::memory_order_relaxed) -
                               g_arm.armNativeDrawsStart;
        const bool mismatch = (delta != g_arm.armValidMeshes);
        if (mismatch) {
            g_armMismatch.fetch_add(1, std::memory_order_relaxed);
        }
        bool printMismatch = false;
        if (mismatch) {
            std::lock_guard<std::mutex> lock(g_mutex);
            printMismatch = g_mismatchPrintedGeoms.insert(g_arm.geometry).second;
        }
        if (g_arm.armIndex == 1 || printMismatch ||
            (g_arm.armIndex != 0 && g_arm.armIndex % 600 == 0)) {
            std::fprintf(stderr,
                         "[nativescene] arm_done geom=%08X meshes=%u valid=%u native_draws=%u arm=%u %s\n",
                         g_arm.geometry, g_arm.armMeshCount, g_arm.armValidMeshes, delta,
                         g_arm.armIndex, mismatch ? "MISMATCH" : "ok");
            std::fflush(stderr);
        }
    }
    g_arm.active = false;
}

void No100fNativeScenePinScanCull(uint32_t atomic) {
    if (!No100fNativeSceneEnabled()) return;
    No100fN9ScopeTimer n9(g_n9PinScanCullNs, g_n9PinScanCullCalls);
    if (!atomic || !Memory::Contains(atomic, 0x70)) return;
    const uint32_t geom = Memory::Read32(atomic + 0x18);
    if (!geom || !Memory::Contains(geom, 0x70)) return;
    PinScan(atomic, geom, "cull", g_scannedCull);
}

void No100fNativeSceneNoteGuestDisplayList(uint32_t listAddr, uint32_t nbytes) {
    if (!g_arm.active) return;
    // Stage 2 (STAGE2-DESK Q3): the note is a per-window census only. It no
    // longer learns and never disables: learning happens at the conversion
    // site (TryGetPacked), where only real draw lists arrive, and DLs that
    // are not the learned identity pass through the player on GX untouched.
    bool already = false;
    for (int i = 0; i < g_arm.censusCount; ++i) {
        if (g_arm.census[i].addr == listAddr && g_arm.census[i].bytes == nbytes) {
            already = true;
            break;
        }
    }
    if (!already) {
        if (g_arm.censusCount < 16) {
            g_arm.census[g_arm.censusCount].addr = listAddr;
            g_arm.census[g_arm.censusCount].bytes = nbytes;
            ++g_arm.censusCount;
        } else if (!g_arm.censusOverflow) {
            g_arm.censusOverflow = true;
            if (!g_censusOverflowLogged) {
                g_censusOverflowLogged = true;
                std::fprintf(stderr,
                             "[nativescene] window census overflow (>16 distinct dls) geom=%08X\n",
                             g_arm.geometry);
                std::fflush(stderr);
            }
        }
    }
    // Informational accounting: count window DLs that are not the armed
    // geometry's learned identity, with a first-sighting line per distinct
    // extra DL so the next boot reveals the full window census.
    bool isLearned = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_caches.find(g_arm.geometry);
        if (it != g_caches.end()) {
            for (const auto& m : it->second.meshes) {
                if (m.dl.learned && m.dl.addr == listAddr && m.dl.bytes == nbytes) {
                    isLearned = true;
                    break;
                }
            }
        }
    }
    if (!isLearned) {
        g_windowExtraDl.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto key = std::make_pair(listAddr, nbytes);
        if (std::find(g_extraDlSeen.begin(), g_extraDlSeen.end(), key) == g_extraDlSeen.end() &&
            g_extraDlSeen.size() < 16) {
            g_extraDlSeen.push_back(key);
            std::fprintf(stderr, "[nativescene] window extra dl ptr=%08X size=%u geom=%08X\n",
                         listAddr, nbytes, g_arm.geometry);
            std::fflush(stderr);
        }
    }
}


// N3b (§3.5): read-only learn-state query for the array-staging skip.
// N5: currently UNCALLED — the fold (gx_dl.cpp) calls TryConvertDl first
// and stages only on failure, so no call site consults this query anymore.
// Left defined for one cycle (revert is a 2-line change per site); delete
// it in the cleanup pass. Its N9 fields keep printing 0/0 so the log
// format stays stable across flights.
// Answers "would TryGetPacked succeed for this DL right now?" without any
// of TryGetPacked's side effects: no first learn, no fingerprint re-learn,
// no semantic counter mutation, no cache invalidation, no log line. (The
// N9 timing counters are the sole exception: they record call time only
// and cannot change the staging decision.) Only the exact (addr, bytes)
// match on a learned mesh can return true; a DL that has never been learned
// returns false and learns in TryGetPacked after staging, exactly as before
// N3b. The generation/digest decision mirrors TryGetPacked step 4 (same
// lazy digest resolve from the probe digest, or from the guest bytes when
// the probe has none). The geometry-level fold guard is deliberately NOT
// evaluated here: FoldGeneration writes the cache's generationTracked flag
// and the guard's invalidation belongs to TryGetPacked. If the query returns
// true but TryGetPacked then fails on that guard, the caller stages the
// arrays and submits GX — the safe direction. A false negative only costs
// one staging.
bool No100fNativeSceneIsLearned(uint32_t listAddr, uint32_t nbytes, uint32_t dlVerts,
                                uint64_t dlDigest, bool dlDigestValid) {
    (void)dlVerts;
    if (!No100fNativeSceneEnabled()) return false;
    No100fN9ScopeTimer n9(g_n9IsLearnedNs, g_n9IsLearnedCalls);
    // Same lazy digest resolve as TryGetPacked: the probe digest is valid
    // on every call after the scan cache's first sighting of this DL; when
    // it is not, compute XXH3_64bits from the guest bytes. Null host
    // pointer means the digest is unavailable.
    uint64_t resolvedDigest = dlDigest;
    bool resolvedValid = dlDigestValid;
    bool resolveAttempted = false;
    // N6: the digest-resolve timing is n9-gated like the scope timers.
    const bool n9On = No100fN9Enabled();
    auto resolveDigest = [&]() -> bool {
        if (resolvedValid || resolveAttempted) return resolvedValid;
        resolveAttempted = true;
        // N9: the digest-resolve path is timed separately from the whole
        // query (same counters as TryGetPacked's resolve lambda).
        const auto t0 = n9On ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
        const uint8_t* host = static_cast<const uint8_t*>(GuestToHostPtr(listAddr, nbytes));
        bool ok = false;
        if (host != nullptr) {
            resolvedDigest = static_cast<uint64_t>(XXH3_64bits(host, static_cast<size_t>(nbytes)));
            resolvedValid = true;
            ok = true;
        }
        if (n9On) {
            g_n9DigestResolveNs.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count()),
                std::memory_order_relaxed);
            g_n9DigestResolveCalls.fetch_add(1, std::memory_order_relaxed);
        }
        return ok;
    };
    std::lock_guard<std::mutex> lock(g_mutex);
    // 1. Exact (addr, bytes) match on any mesh's learned DL (TryGetPacked
    // step 1). No match means not learned yet: return false, do not learn.
    MeshCache* mesh = nullptr;
    Cache* ownerCache = nullptr;
    for (auto& kv : g_caches) {
        for (auto& m : kv.second.meshes) {
            if (m.valid && m.dl.learned && m.dl.addr == listAddr && m.dl.bytes == nbytes) {
                mesh = &m;
                ownerCache = &kv.second;
                break;
            }
        }
        if (mesh != nullptr) break;
    }
    if (mesh == nullptr) return false;
    if (!mesh->valid || !ownerCache->valid) return false;
    // 4. Generation/digest freshness (TryGetPacked step 4). Untracked
    // ranges have no drift check and convert as before.
    if (mesh->dl.generationTracked) {
        const uint64_t curGen = N9GenerationForRange(listAddr, nbytes);
        if (curGen != mesh->dl.generation) {
            // Generation moved: decide by content, as N3a does.
            const bool haveDigest = resolveDigest();
            const uint64_t curDigest = haveDigest ? resolvedDigest : 0;
            if (mesh->dl.digestValid && curDigest != 0 && curDigest == mesh->dl.digest) {
                return true;  // same bytes, rewritten: would retain and convert
            }
            return false;  // no proof, or genuine change: would drift to GX
        }
    }
    return true;
}

void No100fNativeSceneCountStagingSkipped() {
    CountStagingSkipped();
}

// Player-side (gx_dl.cpp): resolves one mesh's packed stream for a learned
// DL (N3). The lookup is per mesh, not per geometry: each mesh carries its
// own learned DL (one DL per mesh, N3 Q2). A DL that arrives at a new
// address with the same (bytes, verts) fingerprint is the same mesh's list
// re-allocated (the diagnostic's 80A7 title set vs the 80B7 menu set) and
// re-learns onto that mesh. A DL that matches no unlearned mesh's vertex
// span is not learned — it plays GX. dlVerts is the player's scan vertex
// span for this DL (maxIdx+1); it is the learn cross-check (a mesh's DL
// spans exactly that mesh's max index + 1) and the re-learn fingerprint.
bool No100fNativeSceneTryGetPacked(uint32_t listAddr, uint32_t nbytes, uint32_t dlVerts,
                                   uint64_t dlDigest, bool dlDigestValid,
                                   const uint8_t** outData, uint32_t* outBytes,
                                   uint16_t* outVertexCount) {
    if (!No100fNativeSceneEnabled()) return false;
    No100fN9ScopeTimer n9(g_n9TryGetPackedNs, g_n9TryGetPackedCalls);
    static thread_local std::vector<uint8_t> copy;
    // N3a: lazy fallback digest (§3.4). The probe digest arrives valid on
    // every call after the scan cache's first sighting of this DL; on the
    // first sighting (no cache entry yet) or the uncached path it is invalid,
    // and TryGetPacked computes it here from the guest bytes instead. Lazy
    // on purpose: resolving runs at most once per call, and only when a learn
    // site or a moved generation actually needs a digest, so DLs that never
    // learn pay no hashing. A null host pointer means the digest is
    // unavailable and the mesh behaves exactly as it did before N3a.
    uint64_t resolvedDigest = dlDigest;
    bool resolvedValid = dlDigestValid;
    bool resolveAttempted = false;
    // N6: the digest-resolve timing is n9-gated like the scope timers.
    const bool n9On = No100fN9Enabled();
    auto resolveDigest = [&]() -> bool {
        if (resolvedValid || resolveAttempted) return resolvedValid;
        resolveAttempted = true;
        // N9: the digest-resolve path is timed separately from the whole
        // call (same counters as IsLearned's resolve lambda).
        const auto t0 = n9On ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
        const uint8_t* host = static_cast<const uint8_t*>(GuestToHostPtr(listAddr, nbytes));
        bool ok = false;
        if (host != nullptr) {
            resolvedDigest = static_cast<uint64_t>(XXH3_64bits(host, static_cast<size_t>(nbytes)));
            resolvedValid = true;
            ok = true;
        }
        if (n9On) {
            g_n9DigestResolveNs.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count()),
                std::memory_order_relaxed);
            g_n9DigestResolveCalls.fetch_add(1, std::memory_order_relaxed);
        }
        return ok;
    };
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        MeshCache* mesh = nullptr;
        Cache* ownerCache = nullptr;
        // 1. Exact (addr, bytes) match on any mesh's learned DL — N5 flat
        // index (O(1) instead of the ~95-entry linear scan). Validates the
        // same conditions as the old scan; a stale entry is erased and
        // falls through to steps 2/3.
        const auto idxIt = g_dlIndex.find(DlIndexKey(listAddr, nbytes));
        if (idxIt != g_dlIndex.end()) {
            const auto ownerIt = g_caches.find(idxIt->second.first);
            if (ownerIt != g_caches.end() && ownerIt->second.valid &&
                idxIt->second.second < ownerIt->second.meshes.size()) {
                MeshCache& m = ownerIt->second.meshes[idxIt->second.second];
                if (m.valid && m.dl.learned && m.dl.addr == listAddr && m.dl.bytes == nbytes) {
                    mesh = &m;
                    ownerCache = &ownerIt->second;
                }
            }
            if (mesh == nullptr) {
                // Stale entry (mesh unlearned / cache replaced / geometry
                // poisoned since the learn): drop it so the next lookup
                // does not pay the validation again.
                g_dlIndex.erase(idxIt);
                g_dlIndexSizeSnap.store(static_cast<uint64_t>(g_dlIndex.size()),
                                        std::memory_order_relaxed);
            }
        }
        // The armed window's census: a DL is only learned (or re-learned)
        // if it was noted inside the last armed window.
        bool inCensus = false;
        if (g_arm.geometry != 0) {
            for (int i = 0; i < g_arm.censusCount; ++i) {
                if (g_arm.census[i].addr == listAddr && g_arm.census[i].bytes == nbytes) {
                    inCensus = true;
                    break;
                }
            }
        }
        // 2. Fingerprint re-learn on the armed geometry: same (bytes,
        // verts) as a learned mesh's DL, new address. Ambiguous
        // fingerprints (two meshes sharing one) do not re-learn.
        if (mesh == nullptr && inCensus && g_arm.geometry != 0) {
            const auto armedIt = g_caches.find(g_arm.geometry);
            if (armedIt != g_caches.end() && armedIt->second.valid) {
                MeshCache* fpCandidate = nullptr;
                uint32_t fpCandidateIndex = 0;
                int fpCount = 0;
                for (uint32_t i = 0; i < armedIt->second.meshes.size(); ++i) {
                    MeshCache& m = armedIt->second.meshes[i];
                    if (m.valid && m.dl.learned && m.dl.bytes == nbytes && m.dl.verts == dlVerts) {
                        fpCandidate = &m;
                        fpCandidateIndex = i;
                        ++fpCount;
                    }
                }
                if (fpCount == 1) {
                    MeshCache& m = *fpCandidate;
                    const uint32_t oldAddr = m.dl.addr;
                    m.dl.addr = listAddr;
                    // N5: the learn moved addresses — move the flat-index
                    // entry with it (the remove only fires if the old key
                    // still maps to this mesh).
                    DlIndexRemove(armedIt->second.geometry, fpCandidateIndex, oldAddr, nbytes);
                    DlIndexAdd(armedIt->second.geometry, fpCandidateIndex, listAddr, nbytes);
                    g_n9Relearns.fetch_add(1, std::memory_order_relaxed);
                    const uint64_t gen = N9GenerationForRange(listAddr, nbytes);
                    m.dl.generationTracked = gen != GxGuestWrite::kUntracked;
                    m.dl.generation = m.dl.generationTracked ? gen : 0;
                    // N3a: re-learn stores the digest alongside the generation,
                    // from the same source as the drift check; if it cannot be
                    // taken, the mesh behaves exactly as it did before N3a.
                    const bool haveDigest = resolveDigest();
                    m.dl.digest = haveDigest ? resolvedDigest : 0;
                    m.dl.digestValid = haveDigest;
                    std::fprintf(stderr,
                                 "[nativescene] relearned dl ptr=%08X size=%u verts=%u geom=%08X (was %08X)\n",
                                 listAddr, nbytes, dlVerts, armedIt->second.geometry, oldAddr);
                    std::fflush(stderr);
                    mesh = &m;
                    ownerCache = &armedIt->second;
                }
            }
        }
        // 3. First learn on the armed geometry: the first unlearned mesh
        // (in mesh order — the guest submits meshes in order) whose vertex
        // span matches this DL's. No match means this DL is not one of the
        // geometry's mesh draws; it plays GX.
        if (mesh == nullptr && inCensus && g_arm.geometry != 0) {
            const auto armedIt = g_caches.find(g_arm.geometry);
            if (armedIt != g_caches.end() && armedIt->second.valid) {
                Cache& c = armedIt->second;
                MeshCache* candidate = nullptr;
                size_t candidateIndex = 0;
                for (size_t i = 0; i < c.meshes.size(); ++i) {
                    MeshCache& m = c.meshes[i];
                    if (!m.valid || m.dl.learned) continue;
                    if (m.maxIndex + 1 == dlVerts) {
                        candidate = &m;
                        candidateIndex = i;
                        break;
                    }
                }
                if (candidate != nullptr) {
                    MeshCache& m = *candidate;
                    m.dl.learned = true;
                    m.dl.addr = listAddr;
                    m.dl.bytes = nbytes;
                    m.dl.verts = dlVerts;
                    // N5: the mesh just learned — publish its flat-index entry.
                    DlIndexAdd(c.geometry, static_cast<uint32_t>(candidateIndex), listAddr, nbytes);
                    const uint64_t gen = N9GenerationForRange(listAddr, nbytes);
                    m.dl.generationTracked = gen != GxGuestWrite::kUntracked;
                    m.dl.generation = m.dl.generationTracked ? gen : 0;
                    // N3a: first learn stores the digest alongside the
                    // generation, from the same source as the drift check;
                    // if it cannot be taken, digestValid=false and the mesh
                    // behaves exactly as it did before N3a.
                    const bool haveDigest = resolveDigest();
                    m.dl.digest = haveDigest ? resolvedDigest : 0;
                    m.dl.digestValid = haveDigest;
                    std::fprintf(stderr,
                                 "[nativescene] learned dl ptr=%08X size=%u verts=%u geom=%08X mesh=%zu\n",
                                 listAddr, nbytes, dlVerts, c.geometry, candidateIndex);
                    std::fflush(stderr);
                    mesh = &m;
                    ownerCache = &c;
                } else {
                    // No unlearned mesh matches: log once per boot (the
                    // counter carries the scale) so a systematic mismatch
                    // is visible without flooding the log.
                    CountFallback(kFbLearnVertsMismatch);
                    static bool logged = false;
                    if (!logged) {
                        logged = true;
                        std::string spans;
                        for (const auto& m : c.meshes) {
                            if (!spans.empty()) spans += ",";
                            spans += std::to_string(m.valid ? m.maxIndex + 1 : 0);
                            if (!m.valid) spans += "(invalid)";
                        }
                        std::fprintf(stderr,
                                     "[nativescene] learn_verts_mismatch geom=%08X dl=%08X size=%u verts=%u mesh_spans=%s\n",
                                     c.geometry, listAddr, nbytes, dlVerts, spans.c_str());
                        std::fflush(stderr);
                    }
                }
            }
        }
        if (mesh == nullptr) return false;  // not a learned list: the normal multi-DL case, GX plays it
        if (!mesh->valid || !ownerCache->valid) {
            CountFallback(kFbDlUnlearned);
            return false;
        }
        if (mesh->dl.generationTracked) {
            const uint64_t curGen = N9GenerationForRange(listAddr, nbytes);
            if (curGen != mesh->dl.generation) {
                // N3a (§3.3): the generation moved. Decide by content, not by the
                // write counter. The guest rebuilds these DLs fresh into the same
                // scratch buffer every frame, so a moved generation is usually a
                // byte-identical rewrite, not a different list.
                const bool haveDigest = resolveDigest();
            const uint64_t curDigest = haveDigest ? resolvedDigest : 0;
            if (mesh->dl.digestValid && curDigest != 0 && curDigest == mesh->dl.digest) {
                // Same bytes, rewritten by the guest's per-frame DL rebuild.
                // Retain the learn and re-arm the fast path.
                mesh->dl.generation = curGen;
                CountDigestRetained();
                // fall through to convert
            } else if (curDigest == 0 || !mesh->dl.digestValid) {
                // No positive proof the bytes match. Take the old behaviour.
                CountDigestUnavailable();
                CountFallback(kFbDlDrift);
                // N5: the mesh just unlearned — drop its flat-index entry.
                DlIndexRemove(ownerCache->geometry,
                              static_cast<uint32_t>(mesh - ownerCache->meshes.data()),
                              listAddr, nbytes);
                mesh->dl.learned = false;
                return false;
            } else {
                // Genuine content change: the list at this address is different.
                CountDigestChanged();
                CountFallback(kFbDlDrift);
                // N5: the mesh just unlearned — drop its flat-index entry.
                DlIndexRemove(ownerCache->geometry,
                              static_cast<uint32_t>(mesh - ownerCache->meshes.data()),
                              listAddr, nbytes);
                mesh->dl.learned = false;
                // Log genuine content changes individually (first 16, then
                // counted only); this is the interesting event.
                if (g_digestChangedLogged.fetch_add(1, std::memory_order_relaxed) < 16) {
                    std::fprintf(stderr,
                                 "[nativescene] digest_changed geom=%08X dl=%08X size=%u verts=%u "
                                 "old_digest=%016llX new_digest=%016llX\n",
                                 ownerCache->geometry, listAddr, nbytes, dlVerts,
                                 static_cast<unsigned long long>(mesh->dl.digest),
                                 static_cast<unsigned long long>(curDigest));
                    std::fflush(stderr);
                }
                return false;
            }
            }
        }
        if (ownerCache->generationTracked && FoldGeneration(*ownerCache) != ownerCache->generation) {
            ownerCache->valid = false;
            // N5: the cache's meshes hold no learned DLs anymore — drop its
            // flat-index entries.
            ClearDlIndexForGeometry(ownerCache->geometry);
            CountFallback(kFbGenChanged);
            return false;
        }
        // N8 C5: exact allowlisted site draw (inside the lock_guard scope;
        // N8GeomNoteLocked takes no lock itself). Identity from the owning
        // cache's geometry (counts re-read) plus its meshCount.
        N8GeomNoteLocked(Memory::Read32(ownerCache->geometry + 0x10),
                         Memory::Read32(ownerCache->geometry + 0x14), ownerCache->meshCount,
                         /*isSiteDraw=*/true);
        copy = mesh->packed;
        *outVertexCount = mesh->vertexCount;
    }
    *outData = copy.data();
    *outBytes = static_cast<uint32_t>(copy.size());
    return true;
}

void No100fNativeSceneCountNativeDraw() {
    if (g_nativeDraws.fetch_add(1, std::memory_order_relaxed) == 0) {
        PrintCounters("first conversion");
    }
}

// ---------------------------------------------------------------------------
// N2 DL census (NATIVE-SCENE-N1-DESIGN.md §4.1): caller-keyed counters in the
// runtime DL path. Gated by the dk_diag key "dlcensus" (NO100F_DLCENSUS);
// absent = off and every entry point below is inert.
//
// Classes keyed by the submission caller — the guest return address at the
// GXCallDisplayList override (no100f_s0_probe.cpp notes it; the player
// consumes the note at submission time, keyed by (listAddr, nbytes),
// last-write-wins). A return address is resolved to its enclosing function by
// scanning back for the standard PPC prologue (mflr r0; stwu r1,-N(r1)).
// Known submitter entries: Atomic func_801402F0, Sector func_80158F70 /
// func_80142B74, Im3D render node 0x80189758. Anything else — including the
// shadow-pass EFB sequence 0x8003B548, Bink and effects — is Other; each
// distinct caller address is logged once at first sighting so the flight
// worker can map it offline (e.g. RwIm2D / camera paths, which have no known
// entry address). GXBegin immediate-mode draws never enter the DL path and
// are not counted at all; the per-window totals line carries the submission
// count so the coverage is auditable.
//
// Metrics per class, per 600-frame window (g_gxFrameCount — the same window
// size as the [vi] phasetime windows): draws = DL submissions whose scan
// found a draw (mayContainDraw); verts = max(maxIdx+1) over indexed attrs
// (the scan's index-count vertex proxy; direct-mode draws contribute 0);
// bytes = nbytes summed over draw-containing DLs (FIFO-intake cost proxy).
// Register-only submissions count only in the subs/subbytes denominator.
// The window print also carries the atomic+sector combined shares D/V/B —
// the N2 kill-criterion inputs — and cumulative totals.

extern "C" int g_gxFrameCount;  // per-presented-frame counter (gx_copy.cpp)

namespace {

enum class DlClass : uint8_t { kAtomic = 0, kSector, kIm3d, kOther, kCount };

constexpr uint32_t kDlCensusAtomicEntry = 0x801402F0u;   // AllInOne body
constexpr uint32_t kDlCensusSectorEntryA = 0x80158F70u;  // WorldSectorAllInOne
constexpr uint32_t kDlCensusSectorEntryB = 0x80142B74u;  // sector submitter
constexpr uint32_t kDlCensusIm3dEntry = 0x80189758u;     // Im3D render node
constexpr uint32_t kDlCensusMaxScanBack = 0x4000u;      // prologue scan window
constexpr size_t kDlCensusMaxNotes = 131072;            // note-table cap
constexpr size_t kDlCensusMaxResolved = 4096;           // lr->entry cache cap
constexpr int kDlCensusMaxCallerLines = 128;            // inventory-line cap
constexpr int kDlCensusWindowFrames = 600;

struct DlCensusBucket {
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t bytes = 0;
};

struct DlCensusWindow {
    DlCensusBucket byClass[static_cast<size_t>(DlClass::kCount)];
    uint64_t subs = 0;       // all guest DL submissions in the window
    uint64_t subBytes = 0;   // nbytes over all submissions
    uint64_t hostSubs = 0;   // SPMDL-stub submissions (host-served; draws unknown)
    uint64_t unnotedDraws = 0;
    uint64_t unnotedVerts = 0;
    uint64_t unnotedBytes = 0;
};

std::unordered_map<uint64_t, DlClass> g_dlCensusNote;  // (addr<<32)|nbytes -> class
std::unordered_map<uint32_t, uint32_t> g_dlCensusResolved;  // lr -> entry (0 = none)
std::unordered_map<uint32_t, bool> g_dlCensusCallerLogged;  // lr -> inventory printed
DlCensusWindow g_dlCensusWindow;
uint64_t g_dlCensusCumDraws = 0;
uint64_t g_dlCensusCumVerts = 0;
uint64_t g_dlCensusCumBytes = 0;
int g_dlCensusWindowIndex = -1;
int g_dlCensusWindowStartFrame = 0;
bool g_dlCensusHeaderPrinted = false;

// N8 C4: per-DL submission-site counts (cumulative). The note side shares
// g_dlCensusNote with the census (same key, same class semantics); the
// accumulate side counts subs/draws per class in its own branch, so the
// [dlcensus] prints stay dlcensus-gated. Indexed by DlClass.
static std::atomic<uint64_t> s_n8SiteSubs[static_cast<size_t>(DlClass::kCount)]{};
static std::atomic<uint64_t> s_n8SiteDraws[static_cast<size_t>(DlClass::kCount)]{};
static std::atomic<uint64_t> s_n8SiteUnnotedSubs{0};
static std::atomic<uint64_t> s_n8SiteUnnotedDraws{0};
static std::atomic<uint64_t> s_n8SiteHostSubs{0};

const char* DlCensusClassName(DlClass cls) {
    switch (cls) {
        case DlClass::kAtomic: return "atomic";
        case DlClass::kSector: return "sector";
        case DlClass::kIm3d: return "im3d";
        default: return "other";
    }
}

DlClass DlCensusClassForEntry(uint32_t entry) {
    if (entry == kDlCensusAtomicEntry) return DlClass::kAtomic;
    if (entry == kDlCensusSectorEntryA || entry == kDlCensusSectorEntryB) return DlClass::kSector;
    if (entry == kDlCensusIm3dEntry) return DlClass::kIm3d;
    return DlClass::kOther;
}

// Resolve a guest return address to its enclosing function entry by scanning
// back for the standard PPC prologue. Returns 0 when no prologue is found
// within the scan window. Results are cached per return address; the direct
// caller of GXCallDisplayList is non-leaf, so its prologue is in range.
uint32_t DlCensusResolveEntry(uint32_t retAddr) {
    if (retAddr < 0x80000000u || retAddr >= 0x81800000u || (retAddr & 3u) != 0) return 0;
    const auto cached = g_dlCensusResolved.find(retAddr);
    if (cached != g_dlCensusResolved.end()) return cached->second;
    uint32_t entry = 0;
    for (uint32_t off = 0; off <= kDlCensusMaxScanBack; off += 4) {
        const uint32_t addr = retAddr - off;
        if (addr < 0x80000000u) break;
        if (!Memory::Contains(addr, 8)) break;
        const uint32_t w0 = Memory::Read32(addr);
        const uint32_t w1 = Memory::Read32(addr + 4);
        const bool mflrFirst = (w0 == 0x7C0802A6u) && ((w1 & 0xFFFF0000u) == 0x94210000u);
        const bool stwuFirst = ((w0 & 0xFFFF0000u) == 0x94210000u) && (w1 == 0x7C0802A6u);
        if (mflrFirst || stwuFirst) {
            entry = addr;
            break;
        }
    }
    if (g_dlCensusResolved.size() < kDlCensusMaxResolved) g_dlCensusResolved[retAddr] = entry;
    return entry;
}

// N8 C4: caller-chain walk from a frame pointer (local copy of the
// S0ProbeCallerChain logic, from no100f_s0_probe.cpp). Used only when the
// direct LR misses a known class, so the common case costs one cached
// lookup instead of the census's unconditional chain walk.
int DlSiteCallerChain(uint32_t frame, uint32_t* chain, int maxChain) {
    int count = 0;
    for (int i = 0; i < maxChain; ++i) {
        if (frame < 0x80000000u || frame >= 0x81800000u || (frame & 3u) != 0) break;
        if (!Memory::Contains(frame, 8)) break;
        const uint32_t next = Memory::Read32(frame);
        if (next < 0x80000000u || next >= 0x81800000u || (next & 3u) != 0) break;
        if (!Memory::Contains(next + 4, 4)) break;
        chain[count++] = Memory::Read32(next + 4);
        frame = next;
    }
    return count;
}

void DlCensusPrintHeaderLocked() {
    if (g_dlCensusHeaderPrinted) return;
    g_dlCensusHeaderPrinted = true;
    std::fprintf(stderr,
                 "[dlcensus] on: window=600 frames (g_gxFrameCount); atomic=801402F0 "
                 "sector=80158F70,80142B74 im3d=80189758; verts=max(maxIdx+1) over indexed "
                 "attrs; bytes=nbytes of draw-containing DLs; GXBegin immediate draws "
                 "are not in this stream\n");
    std::fflush(stderr);
}

// Prints the current window; with reset=true the window buckets are cleared
// afterwards. Caller holds g_mutex.
void DlCensusPrintWindowLocked(bool reset) {
    const DlCensusWindow& w = g_dlCensusWindow;
    if (w.subs == 0) return;
    uint64_t draws = 0, verts = 0, bytes = 0;
    for (size_t i = 0; i < static_cast<size_t>(DlClass::kCount); ++i) {
        draws += w.byClass[i].draws;
        verts += w.byClass[i].verts;
        bytes += w.byClass[i].bytes;
    }
    const auto& a = w.byClass[static_cast<size_t>(DlClass::kAtomic)];
    const auto& s = w.byClass[static_cast<size_t>(DlClass::kSector)];
    const auto& m = w.byClass[static_cast<size_t>(DlClass::kIm3d)];
    const auto& o = w.byClass[static_cast<size_t>(DlClass::kOther)];
    const auto pct = [](uint64_t part, uint64_t total) {
        return total ? 100.0 * static_cast<double>(part) / static_cast<double>(total) : 0.0;
    };
    std::fprintf(stderr,
                 "[dlcensus] window #%d frames %d-%d draws=%llu verts=%llu bytes=%llu subs=%llu "
                 "subbytes=%llu hostsubs=%llu unnoted_d=%llu unnoted_v=%llu unnoted_b=%llu | "
                 "atomic d=%llu (%.1f%%) v=%llu (%.1f%%) b=%llu (%.1f%%) | sector d=%llu "
                 "(%.1f%%) v=%llu (%.1f%%) b=%llu (%.1f%%) | im3d d=%llu (%.1f%%) v=%llu "
                 "(%.1f%%) b=%llu (%.1f%%) | other d=%llu (%.1f%%) v=%llu (%.1f%%) b=%llu "
                 "(%.1f%%) | atomic+sector D=%.1f%% V=%.1f%% B=%.1f%% | cum d=%llu v=%llu "
                 "b=%llu\n",
                 g_dlCensusWindowIndex, g_dlCensusWindowStartFrame,
                 g_dlCensusWindowStartFrame + kDlCensusWindowFrames - 1,
                 static_cast<unsigned long long>(draws), static_cast<unsigned long long>(verts),
                 static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(w.subs),
                 static_cast<unsigned long long>(w.subBytes),
                 static_cast<unsigned long long>(w.hostSubs),
                 static_cast<unsigned long long>(w.unnotedDraws),
                 static_cast<unsigned long long>(w.unnotedVerts),
                 static_cast<unsigned long long>(w.unnotedBytes),
                 static_cast<unsigned long long>(a.draws), pct(a.draws, draws),
                 static_cast<unsigned long long>(a.verts), pct(a.verts, verts),
                 static_cast<unsigned long long>(a.bytes), pct(a.bytes, bytes),
                 static_cast<unsigned long long>(s.draws), pct(s.draws, draws),
                 static_cast<unsigned long long>(s.verts), pct(s.verts, verts),
                 static_cast<unsigned long long>(s.bytes), pct(s.bytes, bytes),
                 static_cast<unsigned long long>(m.draws), pct(m.draws, draws),
                 static_cast<unsigned long long>(m.verts), pct(m.verts, verts),
                 static_cast<unsigned long long>(m.bytes), pct(m.bytes, bytes),
                 static_cast<unsigned long long>(o.draws), pct(o.draws, draws),
                 static_cast<unsigned long long>(o.verts), pct(o.verts, verts),
                 static_cast<unsigned long long>(o.bytes), pct(o.bytes, bytes),
                 pct(a.draws + s.draws, draws), pct(a.verts + s.verts, verts),
                 pct(a.bytes + s.bytes, bytes),
                 static_cast<unsigned long long>(g_dlCensusCumDraws),
                 static_cast<unsigned long long>(g_dlCensusCumVerts),
                 static_cast<unsigned long long>(g_dlCensusCumBytes));
    std::fflush(stderr);
    if (reset) g_dlCensusWindow = DlCensusWindow{};
}

// Advances the 600-frame window when g_gxFrameCount crossed into a new one.
// Caller holds g_mutex.
void DlCensusRolloverLocked() {
    const int frame = g_gxFrameCount;
    const int window = frame >= 0 ? frame / kDlCensusWindowFrames : 0;
    if (g_dlCensusWindowIndex < 0) {
        g_dlCensusWindowIndex = window;
        g_dlCensusWindowStartFrame = window * kDlCensusWindowFrames;
    } else if (window != g_dlCensusWindowIndex) {
        DlCensusPrintWindowLocked(true);
        g_dlCensusWindowIndex = window;
        g_dlCensusWindowStartFrame = window * kDlCensusWindowFrames;
    }
}

}  // namespace

bool No100fDlCensusEnabled() {
    static const bool enabled = [] {
        const char* raw = std::getenv("NO100F_DLCENSUS");
        std::fprintf(stderr, "[dlcensus] gate read: NO100F_DLCENSUS=%s -> %s\n",
                     raw ? raw : "(unset)", raw ? "ON" : "OFF");
        std::fflush(stderr);
        return raw != nullptr;
    }();
    return enabled;
}

void No100fDlCensusNoteCall(uint32_t listAddr, uint32_t nbytes, uint32_t lr,
                            const uint32_t* chain, int chainCount) {
    if (!No100fDlCensusEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    DlCensusPrintHeaderLocked();
    // Direct caller first, then the saved-LR chain (thin wrappers): the first
    // candidate resolving to a known class wins.
    DlClass cls = DlClass::kOther;
    const uint32_t directEntry = DlCensusResolveEntry(lr);
    if (DlCensusClassForEntry(directEntry) != DlClass::kOther) {
        cls = DlCensusClassForEntry(directEntry);
    } else if (chain != nullptr) {
        for (int i = 0; i < chainCount; ++i) {
            const uint32_t entry = DlCensusResolveEntry(chain[i]);
            if (DlCensusClassForEntry(entry) != DlClass::kOther) {
                cls = DlCensusClassForEntry(entry);
                break;
            }
        }
    }
    if (g_dlCensusCallerLogged.size() < static_cast<size_t>(kDlCensusMaxCallerLines) &&
        g_dlCensusCallerLogged.find(lr) == g_dlCensusCallerLogged.end()) {
        g_dlCensusCallerLogged[lr] = true;
        std::fprintf(stderr, "[dlcensus] caller lr=%08X entry=%08X class=%s\n", lr, directEntry,
                     DlCensusClassName(cls));
        std::fflush(stderr);
    }
    if (g_dlCensusNote.size() < kDlCensusMaxNotes) {
        g_dlCensusNote[(static_cast<uint64_t>(listAddr) << 32) | nbytes] = cls;
    }
}

// N8 C4: note a DL submission's caller class for the per-DL submission-site
// counters. Parallel to No100fDlCensusNoteCall, but gated on the nativescene
// key and called from the GXCallDisplayList probe with the direct LR and the
// frame pointer (no chain walk up front). Records into the SHARED
// g_dlCensusNote table (same key, same class semantics); the [dlcensus]
// prints stay dlcensus-gated (no header or caller-inventory print here).
void No100fDlSiteNoteCall(uint32_t listAddr, uint32_t nbytes, uint32_t lr, uint32_t framePtr) {
    if (!No100fNativeSceneEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    DlClass cls = DlClass::kOther;
    const uint32_t directEntry = DlCensusResolveEntry(lr);
    if (DlCensusClassForEntry(directEntry) != DlClass::kOther) {
        cls = DlCensusClassForEntry(directEntry);
    } else {
        uint32_t chain[6] = {};
        const int chainCount = DlSiteCallerChain(framePtr, chain, 6);
        for (int i = 0; i < chainCount; ++i) {
            const uint32_t entry = DlCensusResolveEntry(chain[i]);
            if (DlCensusClassForEntry(entry) != DlClass::kOther) {
                cls = DlCensusClassForEntry(entry);
                break;
            }
        }
    }
    if (g_dlCensusNote.size() < kDlCensusMaxNotes) {
        g_dlCensusNote[(static_cast<uint64_t>(listAddr) << 32) | nbytes] = cls;
    }
}

void No100fDlCensusAccumulate(uint32_t listAddr, uint32_t nbytes, bool drawContaining,
                              uint32_t vertexCount) {
    // N8 C4: per-DL submission-site counts, in its own lock scope (the census
    // body below takes g_mutex again; nesting would deadlock). Gated on the
    // nativescene key, so the toggle set reads zero.
    if (No100fNativeSceneEnabled()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_dlCensusNote.find((static_cast<uint64_t>(listAddr) << 32) | nbytes);
        if (it == g_dlCensusNote.end()) {
            s_n8SiteUnnotedSubs.fetch_add(1, std::memory_order_relaxed);
            if (drawContaining) s_n8SiteUnnotedDraws.fetch_add(1, std::memory_order_relaxed);
        } else {
            const size_t ci = static_cast<size_t>(it->second);
            s_n8SiteSubs[ci].fetch_add(1, std::memory_order_relaxed);
            if (drawContaining) s_n8SiteDraws[ci].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!No100fDlCensusEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    DlCensusPrintHeaderLocked();
    DlCensusRolloverLocked();
    DlCensusWindow& w = g_dlCensusWindow;
    ++w.subs;
    w.subBytes += nbytes;
    if (!drawContaining) return;
    const auto it = g_dlCensusNote.find((static_cast<uint64_t>(listAddr) << 32) | nbytes);
    if (it == g_dlCensusNote.end()) {
        ++w.unnotedDraws;
        w.unnotedVerts += vertexCount;
        w.unnotedBytes += nbytes;
    }
    // Unnoted draws still count toward the totals; they are attributed to
    // Other for the shares, with the unnoted_* fields exposing the amount.
    const DlClass cls = (it == g_dlCensusNote.end()) ? DlClass::kOther : it->second;
    DlCensusBucket& b = w.byClass[static_cast<size_t>(cls)];
    ++b.draws;
    b.verts += vertexCount;
    b.bytes += nbytes;
    ++g_dlCensusCumDraws;
    g_dlCensusCumVerts += vertexCount;
    g_dlCensusCumBytes += nbytes;
}

void No100fDlCensusNoteHostServed(uint32_t listAddr, uint32_t nbytes) {
    // N8 C4: host-served subs (no lock needed: atomic only).
    if (No100fNativeSceneEnabled()) {
        s_n8SiteHostSubs.fetch_add(1, std::memory_order_relaxed);
    }
    if (!No100fDlCensusEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    DlCensusPrintHeaderLocked();
    DlCensusRolloverLocked();
    ++g_dlCensusWindow.subs;
    g_dlCensusWindow.subBytes += nbytes;
    ++g_dlCensusWindow.hostSubs;
}

// N8 C4/C5 getters (cumulative totals since start) and the per-identity
// table print, for the runtime's [n8counters] / [n8geom] lines. The C4
// counters live in the census anonymous namespace above and the C5 map in
// the first one; both are visible at file scope after their namespace
// blocks, in the N9 pattern.
void No100fDlSiteTotals(uint64_t* atomicSubs, uint64_t* atomicDraws, uint64_t* sectorSubs,
                        uint64_t* sectorDraws, uint64_t* im3dSubs, uint64_t* im3dDraws,
                        uint64_t* otherSubs, uint64_t* otherDraws, uint64_t* unnotedSubs,
                        uint64_t* unnotedDraws, uint64_t* hostSubs) {
    const size_t a = static_cast<size_t>(DlClass::kAtomic);
    const size_t s = static_cast<size_t>(DlClass::kSector);
    const size_t m = static_cast<size_t>(DlClass::kIm3d);
    const size_t o = static_cast<size_t>(DlClass::kOther);
    *atomicSubs = s_n8SiteSubs[a].load(std::memory_order_relaxed);
    *atomicDraws = s_n8SiteDraws[a].load(std::memory_order_relaxed);
    *sectorSubs = s_n8SiteSubs[s].load(std::memory_order_relaxed);
    *sectorDraws = s_n8SiteDraws[s].load(std::memory_order_relaxed);
    *im3dSubs = s_n8SiteSubs[m].load(std::memory_order_relaxed);
    *im3dDraws = s_n8SiteDraws[m].load(std::memory_order_relaxed);
    *otherSubs = s_n8SiteSubs[o].load(std::memory_order_relaxed);
    *otherDraws = s_n8SiteDraws[o].load(std::memory_order_relaxed);
    *unnotedSubs = s_n8SiteUnnotedSubs.load(std::memory_order_relaxed);
    *unnotedDraws = s_n8SiteUnnotedDraws.load(std::memory_order_relaxed);
    *hostSubs = s_n8SiteHostSubs.load(std::memory_order_relaxed);
}

void No100fGeomTotals(uint64_t* renders, uint64_t* allowRenders, uint64_t* uncatRenders,
                      uint64_t* unknownRenders, uint64_t* siteDraws, uint64_t* distinct) {
    *renders = s_n8GeomRenders.load(std::memory_order_relaxed);
    *allowRenders = s_n8GeomAllowRenders.load(std::memory_order_relaxed);
    *uncatRenders = s_n8GeomUncatRenders.load(std::memory_order_relaxed);
    *unknownRenders = s_n8GeomUnknownRenders.load(std::memory_order_relaxed);
    *siteDraws = s_n8GeomSiteDraws.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mutex);
    *distinct = static_cast<uint64_t>(s_n8GeomMap.size());
}

void No100fGeomPrintTable() {
    if (!No100fNativeSceneEnabled()) return;
    std::vector<std::pair<N8GeomKey, N8GeomEntry>> rows;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        rows.assign(s_n8GeomMap.begin(), s_n8GeomMap.end());
    }
    std::sort(rows.begin(), rows.end(), [](const std::pair<N8GeomKey, N8GeomEntry>& a,
                                           const std::pair<N8GeomKey, N8GeomEntry>& b) {
        return a.second.renders > b.second.renders;
    });
    std::fprintf(stderr, "[n8geom] %zu identities (renders desc, cap 512)\n", rows.size());
    size_t printed = 0;
    for (const std::pair<N8GeomKey, N8GeomEntry>& row : rows) {
        if (printed >= 512) break;
        std::fprintf(stderr,
                     "[n8geom] countA=%u countB=%u meshCount=%u renders=%llu siteDraws=%llu %s\n",
                     row.first.countA, row.first.countB, row.first.meshCount,
                     static_cast<unsigned long long>(row.second.renders),
                     static_cast<unsigned long long>(row.second.siteDraws),
                     row.second.allowlisted ? "allowlisted" : "uncatalogued");
        ++printed;
    }
    if (rows.size() > printed) {
        std::fprintf(stderr, "[n8geom] ... %zu more identities\n", rows.size() - printed);
    }
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// The render callback itself: a mechanical transliteration of the generated
// shard body for func_801488B8 (see the file header). The only deviation from
// the shard is the arm pair around the RxPipelineExecute call.

extern "C" void No100f_AtomicRenderCallback(CpuContext* ctx) {
    DiagCallbackEntry(ctx->gpr[3]);
    uint32_t cr0_0 = 0;
    uint32_t r4_addr_0 = 0;
    uint32_t r4_addr_1 = 0;
    uint32_t r4_addr_2 = 0;

    uint32_t r0 = ctx->gpr[0];
    uint32_t r1 = ctx->gpr[1];
    uint32_t r3 = ctx->gpr[3];
    uint32_t r4 = ctx->gpr[4];
    uint32_t r5 = ctx->gpr[5];
    uint32_t r13 = ctx->gpr[13];
    uint32_t r31 = ctx->gpr[31];
    uint32_t cr = ctx->cr;
    uint32_t xer = ctx->xer;

    goto loc_801488B8;

loc_801488B8:
{
    MemoryInline::FlatWriteRam32((r1 + -16), r1);
    r1 = (r1 + -16);
    r0 = ctx->lr;
    MemoryInline::FlatWriteRam32((r1 + 20), r0);
    MemoryInline::FlatWriteRam32((r1 + 12), r31);
    r31 = r3;
    r3 = MemoryInline::FlatRead32((r3 + 108));
    SetCRResident(cr, xer, 0, static_cast<uint32_t>(r3), static_cast<uint32_t>(0));
}

loc_801488D4:
{
    if (((cr & 0x20000000u) == 0)) {
        goto loc_801488E8;
    }
}

loc_801488D8:
{
    r3 = MemoryInline::FlatRead32((r13 + -27432));
    r4 = MemoryInline::FlatRead32((r13 + -27652));
    r0 = (r3 + 64);
    r4_addr_1 = (r4 + r0);
    r3 = MemoryInline::FlatRead32(r4_addr_1);
}

loc_801488E8:
{
    r4 = r31;
    r5 = 1;
    ctx->lr = 0x801488F4u;
    ctx->gpr[0] = r0;
    ctx->gpr[1] = r1;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[31] = r31;
    ctx->cr = cr;
    const bool nsArmed = No100fNativeSceneArmBegin(r31);
    func_801850E4(ctx);
    if (nsArmed) No100fNativeSceneArmEnd();
    r0 = ctx->gpr[0];
    r1 = ctx->gpr[1];
    r3 = ctx->gpr[3];
    r4 = ctx->gpr[4];
    r5 = ctx->gpr[5];
    r13 = ctx->gpr[13];
    r31 = ctx->gpr[31];
    cr = ctx->cr;
    xer = ctx->xer;
    SetCRResident(cr, xer, 0, static_cast<uint32_t>(r3), static_cast<uint32_t>(0));
}

loc_801488F8:
{
    if (((cr & 0x20000000u) != 0)) {
        goto loc_80148904;
    }
}

loc_801488FC:
{
    r3 = r31;
    goto loc_80148908;
}

loc_80148904:
{
    r3 = 0;
}

loc_80148908:
{
    r0 = MemoryInline::FlatRead32((r1 + 20));
    r31 = MemoryInline::FlatRead32((r1 + 12));
    ctx->lr = r0;
    r1 = (r1 + 16);
    ctx->gpr[0] = r0;
    ctx->gpr[1] = r1;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[31] = r31;
    ctx->cr = cr;
    return;
}

}

PPC_NATIVE_OVERRIDE_VOID(801488B8, No100f_AtomicRenderCallback, (CpuContext* ctx), (ctx));
