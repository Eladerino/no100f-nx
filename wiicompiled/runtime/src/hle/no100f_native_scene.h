#pragma once
// N8: a macro guard alongside the pragma. The header exists in two copies
// (src/hle and runtime/include); #pragma once is per file, so a unity TU
// that includes both copies (gx_dl.cpp via the include path plus a gx TU
// via the relative path) saw the content twice and failed on the
// redefinition of No100fN9ScopeTimer. The macro guard makes the second
// inclusion a no-op. Both copies stay identical.
#ifndef NO100F_NATIVE_SCENE_H
#define NO100F_NATIVE_SCENE_H

// Native-scene Stage 1 (60 fps campaign, STAGE1-DESIGN.md): intercept one
// pinned static prop atomic, parse its RpGeometry into cached host geometry,
// and submit that geometry natively at the exact stream position of the prop's
// own display list. Gated by the dk_diag key "nativescene"
// (NO100F_NATIVESCENE); absent = off, and every entry point below is inert.

#include <atomic>
#include <chrono>
#include <cstdint>

// The gate, cached from the environment on first use.
bool No100fNativeSceneEnabled();

// N6: the N9 per-call timing gate, cached from the environment on first
// use (dk_diag key "n9" -> NO100F_N9, same handshake as the nativescene
// key in switch/deko/device.cpp). Absent = off: every N9 scope timer is
// a no-op (one static-bool branch per call site) and the n9/n9sub/n9site
// lines do not print. Deliberately silent (no gate-read print): the
// toggle flight's toggle-off check counts exactly one [nativescene] gate
// line, and the n9 state is observable from the n9 lines themselves.
bool No100fN9Enabled();

// Called from the native render-callback override (0x801488B8) around its
// RxPipelineExecute call. ArmBegin returns true only when the gate is on, the
// atomic passes the target pin, and a validated cache exists for that
// geometry pointer (Stage 2 binds identity per pointer); while armed, the
// guest-side GXCallDisplayList override reports the prop's display lists via
// No100fNativeSceneNoteGuestDisplayList. Game thread only.
bool No100fNativeSceneArmBegin(uint32_t atomic);
void No100fNativeSceneArmEnd();

// Diag build: pin-scan entry point for the iModelCull override (the probe's
// traversal point), so one boot shows which geometries each path sees.
void No100fNativeScenePinScanCull(uint32_t atomic);

// Guest-side display-list note, called at the GXCallDisplayList (0x80102464)
// override's entry. Stage 2: records a per-window census only — learning
// happens at the conversion site in No100fNativeSceneTryGetPacked, and
// non-learned lists in a window pass through the player on GX untouched.
void No100fNativeSceneNoteGuestDisplayList(uint32_t listAddr, uint32_t nbytes);

// Player-side (gx_dl.cpp): if (listAddr, nbytes) is a learned target list
// for one mesh of an admitted geometry (learned at this site from the armed
// window's census, guarded by a guest-write generation fold over the DL
// range) and the cache is still generation-valid, copies that mesh's packed
// direct-vertex stream (36-byte vertices: position, normal, colour,
// texcoord0; strip order) into a thread-local buffer and returns it. The
// caller submits it synchronously via aurora::gx::fifo::submit_raw_draw.
// dlVerts is the player's scan vertex span for this DL (maxIdx+1): the
// learn cross-check (a mesh's DL spans exactly its max index + 1) and the
// re-learn fingerprint for a DL re-allocated at a new address. One DL per
// mesh (N3 Q2); the caller submits one draw per DL.
// N3a: dlDigest/dlDigestValid carry the DL scan cache probe's XXH3_64bits
// content digest (0/false when the probe has none, e.g. first sighting).
// When the generation fold has moved, the drift decision is made by content
// digest, not by the write counter: identical bytes retain the learn, only
// a genuine content change drifts to GX (N3-LEARN-RETENTION-DESIGN.md §3).
bool No100fNativeSceneTryGetPacked(uint32_t listAddr, uint32_t nbytes, uint32_t dlVerts,
                                   uint64_t dlDigest, bool dlDigestValid,
                                   const uint8_t** outData, uint32_t* outBytes,
                                   uint16_t* outVertexCount);

// N3b (§3.5): read-only learn-state query for the array-staging skip.
// N5: currently UNCALLED — the fold (gx_dl.cpp) calls TryConvertDl first
// and stages only on failure, so no call site consults this query anymore.
// Left defined for one cycle (revert is a 2-line change per site); delete
// it in the cleanup pass. Its N9 fields keep printing 0/0 so the log
// format stays stable across flights.
// Returns true only when TryGetPacked would succeed for this DL right now —
// an exact (addr, bytes) match on a learned mesh whose cache is valid and
// whose generation/digest check passes. It never learns or re-learns (first
// sightings return false and learn in TryGetPacked after staging, exactly as
// before), never mutates the semantic counters or the cache (the N9 timing
// counters are the sole exception: they record call time only and cannot
// change the staging decision), and never evaluates the geometry-level fold
// (that guard's invalidation belongs to TryGetPacked; a query-true/
// convert-false disagreement is rescued by the staging fallback at the call
// site). A false negative only costs staging; a false positive is safe
// because the caller stages and submits GX when conversion then fails.
bool No100fNativeSceneIsLearned(uint32_t listAddr, uint32_t nbytes, uint32_t dlVerts,
                                uint64_t dlDigest, bool dlDigestValid);

// N3b: counts DLs whose array staging was skipped at a submission site.
// N5: the counter now counts conversions (staging skipped because
// conversion succeeded) — it tracks native_draws by construction, since
// the fold stages only on conversion failure. Expected to track
// native_draws closely; a large gap the other way would mean skips that
// never converted.
void No100fNativeSceneCountStagingSkipped();

// N9 per-call timing counters (N4 diagnosis §3.2, N4-PGO build): total ns +
// call count for each nativescene entry point, the digest-resolve path timed
// separately, GenerationForRange as called from the nativescene path, and
// the bbox falsifier counters (candidate 7). Cheap atomics; one
// steady_clock read + two atomic adds per call (~50-100 ns). N6: every
// scope timer is gated on the n9 dk_diag key (No100fN9Enabled): when the
// key is off the constructor takes no clock read and the destructor is a
// no-op, so the price run pays one static-bool branch per call site.
// Gated on the nativescene dk_diag key like the existing counters;
// printed by PrintCounters (periodic every 6000 arms, N6). The ConvertDl
// pair is declared here because the timer in gx_dl.cpp's
// No100fNativeSceneTryConvertDl needs it; the counters themselves live in
// no100f_native_scene.cpp. The fold-site (n9site) pairs are declared here
// because the timers in gx_dl.cpp's submission sites need them; they live
// in no100f_native_scene.cpp too (N4 precedent).
struct No100fN9ScopeTimer {
    std::atomic<uint64_t>& ns;
    std::atomic<uint64_t>& calls;
    std::chrono::steady_clock::time_point t0;
    bool active;
    No100fN9ScopeTimer(std::atomic<uint64_t>& n, std::atomic<uint64_t>& c)
        : ns(n), calls(c), t0(), active(No100fN9Enabled()) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~No100fN9ScopeTimer() {
        if (!active) return;
        ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - t0).count()),
                     std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
    }
};

extern std::atomic<uint64_t> g_n9TryConvertDlNs;
extern std::atomic<uint64_t> g_n9TryConvertDlCalls;

// N6 fold-site timers (§4.2.4a): pre = the desc/fmt/XF application before
// TryConvertDl at a submission site; post = the forced array re-stage +
// GX submit + state sync after a failed conversion. Printed by
// PrintCounters as "n9site pre_ns/calls post_ns/calls" (only when n9 on).
extern std::atomic<uint64_t> g_n9SitePreNs;
extern std::atomic<uint64_t> g_n9SitePreCalls;
extern std::atomic<uint64_t> g_n9SitePostNs;
extern std::atomic<uint64_t> g_n9SitePostCalls;

// N9 bbox falsifier (N4 diagnosis §4 candidate 7): counts GXClearBoundingBox
// calls and [gxbbox] reads per boot. Both are inert when the nativescene
// key is off. Called from the GX bbox code, not from the nativescene path.
void No100fNativeSceneCountBboxClear();
void No100fNativeSceneCountBboxRead();

// Bookkeeping from the player after a successful native submission.
void No100fNativeSceneCountNativeDraw();

// N2 DL census (NATIVE-SCENE-N1-DESIGN.md §4.1): caller-keyed counters in the
// runtime DL path. Gated by the dk_diag key "dlcensus" (NO100F_DLCENSUS);
// absent = off, and every entry point below is inert. Classes keyed by the
// submission caller (guest return address at the GXCallDisplayList override).
bool No100fDlCensusEnabled();

// Called from the GXCallDisplayList (0x80102464) override's entry with the
// guest return address and the saved-LR caller chain (same walk as the Stage 0
// probe). Records the caller's class for (listAddr, nbytes); the player
// consumes it at submission time. Game thread only.
void No100fDlCensusNoteCall(uint32_t listAddr, uint32_t nbytes, uint32_t lr,
                            const uint32_t* chain, int chainCount);

// Called from the DL player (gx_dl.cpp) once per guest DL submission, after
// the scan established mayContainDraw and maxIdx. drawContaining=false counts
// a register-only submission (denominator only). vertexCount is the scan's
// max-index vertex count for this DL (0 when not draw-containing).
void No100fDlCensusAccumulate(uint32_t listAddr, uint32_t nbytes, bool drawContaining,
                              uint32_t vertexCount);

// Called from the DL player for a submission served from a host-recorded
// display list (SPMDL stub): counted in the submission denominator only —
// its draw content is host-encoded and not scanned.
void No100fDlCensusNoteHostServed(uint32_t listAddr, uint32_t nbytes);

// N8 counters (Step 2 measurement build; see N8-COUNTER-BUILD-LOG.md).
// Gated by the dk_diag key "nativescene" (NO100F_NATIVESCENE); absent = off,
// and every entry point below is inert. C4: per-DL submission-site counts
// (same classes as the census). C5: per-geometry recurrence.
void No100fDlSiteNoteCall(uint32_t listAddr, uint32_t nbytes, uint32_t lr, uint32_t framePtr);
void No100fDlSiteTotals(uint64_t* atomicSubs, uint64_t* atomicDraws, uint64_t* sectorSubs,
                        uint64_t* sectorDraws, uint64_t* im3dSubs, uint64_t* im3dDraws,
                        uint64_t* otherSubs, uint64_t* otherDraws, uint64_t* unnotedSubs,
                        uint64_t* unnotedDraws, uint64_t* hostSubs);
void No100fGeomTotals(uint64_t* renders, uint64_t* allowRenders, uint64_t* uncatRenders,
                      uint64_t* unknownRenders, uint64_t* siteDraws, uint64_t* distinct);
void No100fGeomPrintTable();

#endif  // NO100F_NATIVE_SCENE_H
