// Native-scene Stage 1+2 (60 fps campaign, per STAGE1-DESIGN.md and
// STAGE2-DESK.md): intercept pinned static prop atomics on the Mystic Manor
// route — geometry counts 302/198, texture "rma00001" (route census in the
// s1e/s1diag logs) — parse each RpGeometry once into per-pointer cached
// host geometry, and let the display-list player submit that geometry
// natively at the exact stream position of the prop's own display list.
// Everything else stays on the GX path.
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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <string>
#include <vector>

// Generated callee, declared directly (runtime TUs get no generated header).
extern "C" void func_801850E4(CpuContext* ctx);

namespace {

// Pin evidence (Stage 2, STAGE2-DESK.md): the route census in the s1e and
// s1diag logs (28 geometries first seen after the h001.HIP load read, all
// w18=1/w1C=1, machine-checked) makes rma00001 the strongest manor-route
// target: counts 302/198, [geom+0x24]==1 (single mesh), texture
// "rma00001", seen at both pin-scan sources in both boots. It is also the
// only route geometry with direct multi-instance proof — a second heap
// instance (0x810691A0) with identical counts/structure/texture appeared
// in s1diag — which is why Stage 2 binds identity per geometry pointer
// instead of one pointer per boot. [geom+0x24] tracking the mesh count in
// this fork was proven by the Nov 16 diag boot pin-scan over 45
// geometries. The earlier baseplate_12 pin (204/200, 5 meshes) engaged on
// hardware but is a title/menu prop; it is superseded by this target.
constexpr uint32_t kPinCountA = 302;  // [geom+0x10]
constexpr uint32_t kPinCountB = 198;  // [geom+0x14] numVertices
constexpr uint32_t kPinMeshCount = 1;  // [geom+0x24] and mesh-header mesh count
constexpr const char* kPinTextureName = "rma00001";  // material-chain texture name
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
    kFbCount
};

std::atomic<uint32_t> g_fallbacks[kFbCount]{};
std::atomic<uint32_t> g_nativeDraws{0};
std::atomic<uint32_t> g_cacheBuilds{0};
std::atomic<uint32_t> g_armCalls{0};
std::atomic<uint32_t> g_windowExtraDl{0};  // informational: window DLs that are not the learned identity

void PrintCounters(const char* why);

void CountFallback(Fallback f) {
    // Print on the first occurrence of each kind: a boot where the pin never
    // fully passes must not be silent (the 812f3f9e parity boot was — counted
    // fallbacks printed nothing because PrintCounters only fired on success).
    if (g_fallbacks[f].fetch_add(1, std::memory_order_relaxed) == 0) {
        PrintCounters("first fallback");
    }
}

void PrintCounters(const char* why) {
    std::fprintf(stderr,
                 "[nativescene] counters (%s): native_draws=%u cache_builds=%u arms=%u "
                 "fallbacks pin_structure=%u name_mismatch=%u parse_invalid=%u gen_changed=%u "
                 "dl_unlearned=%u dl_drift=%u interp_on=%u ambiguous=%u window_extra_dl=%u\n",
                 why, g_nativeDraws.load(), g_cacheBuilds.load(), g_armCalls.load(),
                 g_fallbacks[kFbPinStructure].load(), g_fallbacks[kFbNameMismatch].load(),
                 g_fallbacks[kFbParseInvalid].load(), g_fallbacks[kFbGenChanged].load(),
                 g_fallbacks[kFbDlUnlearned].load(), g_fallbacks[kFbDlDrift].load(),
                 g_fallbacks[kFbInterpOn].load(), g_fallbacks[kFbAmbiguous].load(),
                 g_windowExtraDl.load());
    std::fflush(stderr);
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

// Walks geometry -> mesh header -> mesh[0] material -> texture and reads the
// texture name (the chain the parse validates; shipping backdrop code proves
// [matList][0] -> [material+0] -> name at +0x10).
bool ReadTextureNameViaMesh(uint32_t geom, std::string& out) {
    if (!Memory::Contains(geom, 0x70)) return false;
    const uint32_t meshHeader = Memory::Read32(geom + 0x60);
    if (!meshHeader || !Memory::Contains(meshHeader, 16)) return false;
    const uint32_t meshBase = meshHeader + 16 + Memory::Read32(meshHeader + 0xC);
    if (!Memory::Contains(meshBase, 12)) return false;
    const uint32_t material = Memory::Read32(meshBase + 8);
    if (!material || !Memory::Contains(material, 4)) return false;
    const uint32_t texture = Memory::Read32(material);
    if (!texture || !Memory::Contains(texture, 0x20)) return false;
    return ReadGuestName(texture + 0x10, out);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Cache. Stage 2 binds identity per geometry pointer: every distinct
// geometry passing the full pin gets its own entry, learned DL and arm
// accounting (rma00001 legitimately exists as two heap instances).

// Learned display-list identity for one geometry (STAGE2-DESK Q3):
// (pointer, size) plus a guest-write generation fold over the DL range as
// a content guard. Untracked ranges fall back to (pointer, size) alone —
// the Stage 1 behaviour, and the documented residual risk.
struct DlIdentity {
    bool learned = false;
    uint32_t addr = 0;
    uint32_t bytes = 0;
    uint64_t generation = 0;
    bool generationTracked = false;
};

struct Cache {
    bool valid = false;
    bool poisoned = false;
    uint32_t geometry = 0;
    uint16_t serial = 0;
    uint64_t generation = 0;
    bool generationTracked = false;
    std::string textureName;
    // Fold inputs, kept so the generation can be re-sampled verbatim.
    uint32_t meshHeader = 0, positions = 0, normals = 0, colours = 0, uv = 0, indexData = 0;
    uint32_t numVertices = 0, indexCount = 0;
    std::vector<uint8_t> packed;
    uint16_t vertexCount = 0;
    DlIdentity dl;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Cache> g_caches;
std::vector<uint32_t> g_boundGeometries;    // first-binding audit log (per pointer)
std::vector<uint32_t> g_engagedGeometries;  // engaged line printed once per geometry
std::vector<std::pair<uint32_t, uint32_t>> g_extraDlSeen;  // first-sighting lines, capped at 16
bool g_censusOverflowLogged = false;

uint64_t FoldGeneration(Cache& c) {
    using GxGuestWrite::GenerationForRange;
    using GxGuestWrite::kUntracked;
    uint64_t folded = 0;
    bool tracked = true;
    const auto add = [&](uint32_t addr, uint32_t nbytes) {
        const uint64_t v = GenerationForRange(addr, nbytes);
        if (v == kUntracked) {
            tracked = false;
        } else {
            folded += v;
        }
    };
    add(c.geometry, 0x70);
    add(c.meshHeader, 16 + 12);
    add(c.positions, c.numVertices * 12);
    add(c.normals, c.numVertices * 12);
    add(c.colours, c.numVertices * 4);
    add(c.uv, c.numVertices * 8);
    add(c.indexData, c.indexCount * 2);
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
// sets *reason; the caller poisons the geometry. g_mutex held by the caller.
bool ParseGeometry(uint32_t atomic, uint32_t geom, Cache& out, const char** reason) {
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
    if (meshCount != kPinMeshCount) return fail("mesh_count");
    const uint32_t meshBase = meshHeader + 16 + Memory::Read32(meshHeader + 0xC);
    if (!Memory::Contains(meshBase, 12)) return fail("mesh_extent");
    next.indexData = Memory::Read32(meshBase);
    next.indexCount = Memory::Read32(meshBase + 4);
    const uint32_t meshMaterial = Memory::Read32(meshBase + 8);
    if (next.indexCount < 3 || next.indexCount > 65535) return fail("index_count");
    if (!next.indexData || !Memory::Contains(next.indexData, next.indexCount * 2)) return fail("index_extent");
    if (!meshMaterial || !Memory::Contains(meshMaterial, 4)) return fail("material_ptr");
    const uint32_t texture = Memory::Read32(meshMaterial);
    if (!texture || !Memory::Contains(texture, 0x54)) return fail("texture_ptr");
    if (!ReadGuestName(texture + 0x10, next.textureName)) return fail("texture_name");
    const uint32_t raster = Memory::Read32(texture);
    if (!raster || !Memory::Contains(raster, 0x14)) return fail("raster_ptr");
    const uint32_t cpPixels = Memory::Read32(raster + 4);
    const uint32_t texW = Memory::Read32(raster + 0x0C);
    const uint32_t texH = Memory::Read32(raster + 0x10);

    // Validation battery: the parse is untrusted until all of this passes.
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

    std::vector<uint16_t> indices(next.indexCount);
    uint32_t maxIndex = 0;
    for (uint32_t i = 0; i < next.indexCount; ++i) {
        indices[i] = ReadGuest16(next.indexData + i * 2);
        if (indices[i] >= numVertices) return fail("index_out_of_range");
        if (indices[i] > maxIndex) maxIndex = indices[i];
    }
    if (numVertices >= 3 && maxIndex < numVertices - 2) return fail("index_histogram");

    // Pack the strip-expanded direct-vertex stream (big-endian GX format,
    // the same byte order submit_raw_draw's other caller packs).
    next.packed.reserve(static_cast<size_t>(next.indexCount) * kVertexBytes);
    for (uint32_t s = 0; s < next.indexCount; ++s) {
        const uint32_t i = indices[s];
        AppendBEFloat(next.packed, pos[i * 3]);
        AppendBEFloat(next.packed, pos[i * 3 + 1]);
        AppendBEFloat(next.packed, pos[i * 3 + 2]);
        AppendBEFloat(next.packed, nrm[i * 3]);
        AppendBEFloat(next.packed, nrm[i * 3 + 1]);
        AppendBEFloat(next.packed, nrm[i * 3 + 2]);
        for (int b = 0; b < 4; ++b) next.packed.push_back(Memory::Read8(next.colours + i * 4 + b));
        AppendBEFloat(next.packed, Memory::ReadFloat32(next.uv + i * 8));
        AppendBEFloat(next.packed, Memory::ReadFloat32(next.uv + i * 8 + 4));
    }
    next.vertexCount = static_cast<uint16_t>(next.indexCount);
    next.generation = FoldGeneration(next);
    next.valid = true;

    if (std::find(g_engagedGeometries.begin(), g_engagedGeometries.end(), geom) ==
        g_engagedGeometries.end()) {
        g_engagedGeometries.push_back(geom);
        std::fprintf(stderr,
                     "[nativescene] engaged geom=%08X atomic=%08X counts=%u/%u morph=%u mats=%u "
                     "meshes=%u serial=%u tex=\"%s\" raster=%08X cpPixels=%08X %ux%u pos=%08X nrm=%08X "
                     "clr=%08X uv=%08X idx=%08X nidx=%u expected_addr_match=%d\n",
                     geom, atomic, Memory::Read32(geom + 0x10), numVertices,
                     Memory::Read32(geom + 0x18), Memory::Read32(geom + 0x24), meshCount,
                     next.serial, next.textureName.c_str(), raster, cpPixels, texW, texH,
                     next.positions, next.normals, next.colours, next.uv, next.indexData,
                     next.indexCount, geom == kExpectedGeometry ? 1 : 0);
        std::fprintf(stderr,
                     "[nativescene] first tri idx=%u,%u,%u v0 pos=(%.3f,%.3f,%.3f) nrm=(%.3f,%.3f,%.3f) "
                     "uv=(%.3f,%.3f)\n",
                     indices[0], indices[1], indices[2], pos[0], pos[1], pos[2], nrm[0], nrm[1],
                     nrm[2], Memory::ReadFloat32(next.uv), Memory::ReadFloat32(next.uv + 4));
        std::fflush(stderr);
    }

    out = std::move(next);
    return true;
}

// g_mutex held by the caller.
bool EnsureCacheLocked(uint32_t atomic, uint32_t geom) {
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
        CountFallback(kFbGenChanged);
    }
    const char* reason = nullptr;
    Cache next;
    if (ParseGeometry(atomic, geom, next, &reason)) {
        g_cacheBuilds.fetch_add(1, std::memory_order_relaxed);
        g_caches[geom] = std::move(next);
        return true;
    }
    Cache poison;
    poison.poisoned = true;
    poison.geometry = geom;
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
    struct CensusEntry {
        uint32_t addr = 0;
        uint32_t bytes = 0;
    };
    CensusEntry census[8];
    int censusCount = 0;
    bool censusOverflow = false;
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
    const char* verdict = "PASS";
    if (countA != kPinCountA || countB != kPinCountB) {
        verdict = "FAIL counts";
    } else if (w18 != 1) {
        verdict = "FAIL morph_w18";
    } else if (w24 != kPinMeshCount) {
        verdict = "FAIL materials_w24";
    } else if (nameOk && texName != kPinTextureName) {
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
    if (atomic && Memory::Contains(atomic, 0x70)) {
        geom = Memory::Read32(atomic + 0x18);
        if (geom && Memory::Contains(geom, 0x70)) {
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
}

}  // namespace

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
    if (g_arm.active) return false;  // no nesting; the execute call is not re-entrant here
    if (RuntimeConfigFile::FrameInterpolationFps() != 0) {
        CountFallback(kFbInterpOn);
        return false;
    }
    if (!atomic || !Memory::Contains(atomic, 0x70)) return false;
    const uint32_t geom = Memory::Read32(atomic + 0x18);
    if (!geom || !Memory::Contains(geom, 0x70)) return false;
    if (Memory::Read32(geom + 0x10) != kPinCountA || Memory::Read32(geom + 0x14) != kPinCountB) {
        return false;  // not the prop: silent, this is every other atomic
    }
    if (Memory::Read32(geom + 0x18) != 1 || Memory::Read32(geom + 0x24) != kPinMeshCount) {
        CountFallback(kFbPinStructure);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Per-pointer identity (Stage 2): every distinct geometry passing
        // the full pin gets its own cache entry and learned DL. The first
        // binding per pointer is logged so bindings stay auditable; there
        // is no boot-fatal ambiguity state.
        if (std::find(g_boundGeometries.begin(), g_boundGeometries.end(), geom) ==
            g_boundGeometries.end()) {
            g_boundGeometries.push_back(geom);
            std::fprintf(stderr, "[nativescene] bound geom=%08X tex=\"%s\" (binding #%zu)\n",
                         geom, kPinTextureName, g_boundGeometries.size());
            std::fflush(stderr);
        }
        const auto boundIt = g_caches.find(geom);
        if (boundIt != g_caches.end() && boundIt->second.poisoned) return false;
        std::string pinName;
        if (!ReadTextureNameViaMesh(geom, pinName) || pinName != kPinTextureName) {
            CountFallback(kFbNameMismatch);
            std::fprintf(stderr,
                         "[nativescene] name_mismatch geom=%08X expected=\"%s\" actual=\"%s\"; "
                         "geometry poisoned for this boot (GX fallback)\n",
                         geom, kPinTextureName, pinName.empty() ? "?" : pinName.c_str());
            std::fflush(stderr);
            Cache poison;
            poison.poisoned = true;
            poison.geometry = geom;
            g_caches[geom] = std::move(poison);
            return false;
        }
        if (!EnsureCacheLocked(atomic, geom)) return false;
        std::string nowName;
        const auto entryIt = g_caches.find(geom);
        if (entryIt == g_caches.end() || !ReadTextureNameViaMesh(geom, nowName) ||
            nowName != entryIt->second.textureName) {
            CountFallback(kFbNameMismatch);
            return false;
        }
    }
    g_arm.censusCount = 0;
    g_arm.censusOverflow = false;
    g_arm.active = true;
    g_arm.atomic = atomic;
    g_arm.geometry = geom;
    if (g_armCalls.fetch_add(1, std::memory_order_relaxed) % 600 == 599) {
        PrintCounters("periodic");
    }
    return true;
}

void No100fNativeSceneArmEnd() { g_arm.active = false; }

void No100fNativeScenePinScanCull(uint32_t atomic) {
    if (!No100fNativeSceneEnabled()) return;
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
        if (g_arm.censusCount < 8) {
            g_arm.census[g_arm.censusCount].addr = listAddr;
            g_arm.census[g_arm.censusCount].bytes = nbytes;
            ++g_arm.censusCount;
        } else if (!g_arm.censusOverflow) {
            g_arm.censusOverflow = true;
            if (!g_censusOverflowLogged) {
                g_censusOverflowLogged = true;
                std::fprintf(stderr,
                             "[nativescene] window census overflow (>8 distinct dls) geom=%08X\n",
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
        if (it != g_caches.end() && it->second.dl.learned && it->second.dl.addr == listAddr &&
            it->second.dl.bytes == nbytes) {
            isLearned = true;
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


bool No100fNativeSceneTryGetPacked(uint32_t listAddr, uint32_t nbytes, const uint8_t** outData,
                                   uint32_t* outBytes, uint16_t* outVertexCount) {
    if (!No100fNativeSceneEnabled()) return false;
    static thread_local std::vector<uint8_t> copy;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Cache* owner = nullptr;
        for (auto& kv : g_caches) {
            Cache& candidate = kv.second;
            if (candidate.dl.learned && candidate.dl.addr == listAddr &&
                candidate.dl.bytes == nbytes) {
                owner = &candidate;
                break;
            }
        }
        if (!owner && g_arm.geometry != 0) {
            // Learning at the conversion site (STAGE2-DESK Q3 item 2): this
            // DL survived the player's scan+apply, so it is a real draw
            // list; if it was noted inside the last armed window, it is
            // that geometry's DL. Learn it and convert this submission.
            bool inCensus = false;
            for (int i = 0; i < g_arm.censusCount; ++i) {
                if (g_arm.census[i].addr == listAddr && g_arm.census[i].bytes == nbytes) {
                    inCensus = true;
                    break;
                }
            }
            const auto it = g_caches.find(g_arm.geometry);
            if (inCensus && it != g_caches.end() && it->second.valid && !it->second.dl.learned) {
                Cache& c = it->second;
                c.dl.learned = true;
                c.dl.addr = listAddr;
                c.dl.bytes = nbytes;
                // Content guard: fold the guest-write generation over the
                // DL's byte range (the GxGuestWrite machinery FoldGeneration
                // uses for geometry). Untracked ranges fall back to
                // (pointer, size) alone, documented above.
                const uint64_t gen = GxGuestWrite::GenerationForRange(listAddr, nbytes);
                c.dl.generationTracked = gen != GxGuestWrite::kUntracked;
                c.dl.generation = c.dl.generationTracked ? gen : 0;
                std::fprintf(stderr, "[nativescene] learned dl ptr=%08X size=%u geom=%08X\n",
                             listAddr, nbytes, c.geometry);
                std::fflush(stderr);
                owner = &c;
            }
        }
        if (!owner) return false;  // not a learned list: the normal multi-DL case, GX plays it
        if (!owner->valid) {
            CountFallback(kFbDlUnlearned);
            return false;
        }
        if (owner->dl.generationTracked &&
            GxGuestWrite::GenerationForRange(listAddr, nbytes) != owner->dl.generation) {
            // Per-DL, recoverable drift (Stage 2): unlearn this DL only;
            // this submission plays GX and the next armed window re-learns.
            // Never a boot state; the geometry cache is untouched.
            CountFallback(kFbDlDrift);
            owner->dl.learned = false;
            return false;
        }
        if (owner->generationTracked && FoldGeneration(*owner) != owner->generation) {
            owner->valid = false;
            CountFallback(kFbGenChanged);
            return false;
        }
        copy = owner->packed;
        *outVertexCount = owner->vertexCount;
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

void No100fDlCensusAccumulate(uint32_t listAddr, uint32_t nbytes, bool drawContaining,
                              uint32_t vertexCount) {
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
    if (!No100fDlCensusEnabled()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    DlCensusPrintHeaderLocked();
    DlCensusRolloverLocked();
    ++g_dlCensusWindow.subs;
    g_dlCensusWindow.subBytes += nbytes;
    ++g_dlCensusWindow.hostSubs;
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
