#pragma once

// Native-scene Stage 1 (60 fps campaign, STAGE1-DESIGN.md): intercept one
// pinned static prop atomic, parse its RpGeometry into cached host geometry,
// and submit that geometry natively at the exact stream position of the prop's
// own display list. Gated by the dk_diag key "nativescene"
// (NO100F_NATIVESCENE); absent = off, and every entry point below is inert.

#include <cstdint>

// The gate, cached from the environment on first use.
bool No100fNativeSceneEnabled();

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
// for one of the pinned geometries (learned at this site from the armed
// window's census, guarded by a guest-write generation fold over the DL
// range) and the cache is still generation-valid, copies the packed direct-vertex
// stream (36-byte vertices: position, normal, colour, texcoord0; strip order)
// into a thread-local buffer and returns it. The caller submits it
// synchronously via aurora::gx::fifo::submit_raw_draw.
bool No100fNativeSceneTryGetPacked(uint32_t listAddr, uint32_t nbytes,
                                   const uint8_t** outData, uint32_t* outBytes,
                                   uint16_t* outVertexCount);

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
