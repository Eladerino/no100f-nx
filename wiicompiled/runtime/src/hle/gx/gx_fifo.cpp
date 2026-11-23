#include "gx_internal.h"
#include "../no100f_native_scene.h"
#include "gx_stream_common.h"
#include "gx_cp_decode.h"
#include "isa/big_endian.h"
#include "recomp_mod_loader.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

// Opcode constants and the stream helpers this file shares with gx_dl.cpp /
// gx_vertex.cpp; see gx_stream_common.h.
using namespace GxCmd;
using namespace GxStream;

HleGxState g_hleGxState;
// Last PE token (BP 0x48) seen in the gather pipe. Draws are consumed in order, so it is reached at once.
uint16_t g_gxPeToken = 0;

// NO100F GXReadDrawSync: reads __peReg[7]; no PE is emulated, so the boot GXInit wait spun forever.
extern "C" uint32_t No100f_GXReadDrawSync() { return g_gxPeToken; }
PPC_NATIVE_OVERRIDE(800FDE88, No100f_GXReadDrawSync, uint32_t, (), ());

void QueueGuestCallback(uint32_t callbackPtr, uint32_t r3, uint32_t r4);
void QueueGxInterruptCallback(uint32_t callbackPtr, uint32_t r3, uint32_t r4);  // hle/storage/dvd.cpp

// Follow-up instrument (SEAMFIX-FOLLOWUP-STUDY, Nov 16, 2026): per-boot counters
// for the two GX completion sites - No100f_GXEnableBreakPt calls and
// RaisePeInterruptForBp hits by register - so one storm boot separates "the guest
// stops calling" from "the FIFO stops reaching the site" and sizes how often the
// PE path fires in this title. Pure observation: relaxed atomics, no guest-visible
// state; the getters are read by the tracecnt sampler in hle/vi.cpp ([gxcnt]).
static std::atomic<uint32_t> g_gxBreakPtCalls{0};
static std::atomic<uint32_t> g_peHitsToken{0};     // reg 0x47: token + interrupt
static std::atomic<uint32_t> g_peHitsDrawDone{0};  // reg 0x45: draw done
static std::atomic<uint32_t> g_peHitsSync{0};      // reg 0x48: PE token write
static std::atomic<uint32_t> g_peHitsOther{0};     // every other BP register
uint32_t GxFifo_HLE_BreakPtCalls() { return g_gxBreakPtCalls.load(std::memory_order_relaxed); }
uint32_t GxFifo_HLE_PeHitsToken() { return g_peHitsToken.load(std::memory_order_relaxed); }
uint32_t GxFifo_HLE_PeHitsDrawDone() { return g_peHitsDrawDone.load(std::memory_order_relaxed); }
uint32_t GxFifo_HLE_PeHitsSync() { return g_peHitsSync.load(std::memory_order_relaxed); }
uint32_t GxFifo_HLE_PeHitsOther() { return g_peHitsOther.load(std::memory_order_relaxed); }

// Fix 2 (FIX2-DESIGN-STUDY Part B): per-present completion flag. Set at the two
// queue sites in this file that queue a frame-swap completion -
// No100f_GXEnableBreakPt below and the RaisePeInterruptForBp draw-done branch -
// regardless of which deque the entry takes; read and cleared only by the Fix 2
// present hook in hle/vi.cpp, once per present, as the input to its synthesis
// decision. A set flag at the hook means a genuine completion was queued in
// the current present interval, so the hook must not synthesize a second one;
// a genuine call landing after the hook sets the flag for the next interval
// and can only suppress one synthesis there, never double one. Host-side
// bookkeeping only: a relaxed atomic, no guest-visible state.
static std::atomic<bool> g_gxPresentCompletionQueued{false};
void GxFifo_HLE_NotePresentCompletion() {
    g_gxPresentCompletionQueued.store(true, std::memory_order_relaxed);
}
bool GxFifo_HLE_TakePresentCompletion() {
    return g_gxPresentCompletionQueued.exchange(false, std::memory_order_relaxed);
}

// The PE interrupts a raw BP write raises on hardware, delivered like __GXTokenInterruptHandler /
// __GXFinishInterruptHandler would (NO100F globals). Draws are consumed in order, so they fire at once.
// The engine's frame swap sleeps until its GXSetDrawSyncCallback callback sees the frame's token.
// NO100F GXEnableBreakPt: the engine's frame swap sets a CP FIFO breakpoint at the write pointer and sleeps until
// GXCPInterruptHandler calls the GXSetBreakPtCallback callback. The FIFO is consumed as it is written, so the
// breakpoint is already reached: deliver the callback (no arguments) at once.
// seamfix reroute (SEAMFIX-FOLLOWUP-STUDY fix 1, Nov 16, 2026): the callback read
// here is the display-sync advancer (func_80175AA4) - the frame-swap completion the
// display-sync park turns on. The seam build held only the RaisePeInterruptForBp
// class, which never fired in its proving boot; this site queued the advancer into
// the general deque, whose restore-time deliveries spent it on the awake waiter.
// When seamfix is armed the callback joins the held GX class instead, so the seam
// pass delivers it after the waiter links; unarmed it takes the general deque
// exactly as before.
extern "C" void No100f_GXEnableBreakPt(uint32_t breakPt) {
    (void)breakPt;
    g_gxBreakPtCalls.fetch_add(1, std::memory_order_relaxed);
    constexpr uint32_t kBreakPointCallbackAddr = 0x802712A4u;  // BreakPointCB (r13-0x6FDC)
    uint32_t cb = 0;
    if (Memory::TryRead32(kBreakPointCallbackAddr, cb) && cb) {
        if (OS_HLE_SeamFixArmed()) QueueGxInterruptCallback(cb, 0, 0);
        else QueueGuestCallback(cb, 0, 0);
        GxFifo_HLE_NotePresentCompletion();  // Fix 2: a frame-swap completion was queued in this present interval
    }
}
PPC_NATIVE_OVERRIDE_VOID(800FCBDC, No100f_GXEnableBreakPt, (uint32_t breakPt), (breakPt));

// NO100F GXBeginDisplayList / GXEndDisplayList: the engine compiles geometry into display lists at load time.
// The game's own versions point the CPU FIFO at the list buffer, which the hooked GXSetCPUFifo cannot follow, so
// the commands were executed instead of recorded and the size came out as garbage. Record the gather-pipe bytes
// into the guest buffer as hardware does; GXCallDisplayList replays them through this parser.
// Like the SDK, pending dirty state (__GXData->dirtyState, +0x4F4) is flushed to the main FIFO before recording starts.
// The engine's list builder (0x8015E944) sizes each list for the draw alone and lays the vertex arrays out right
// after it; state recorded at the list's GXBegin pushed the last indices into those arrays, which then overwrote them.
extern "C" void func_800FE204(CpuContext* ctx);  // the game's __GXSetDirtyState
extern "C" void No100f_GXBeginDisplayList(uint32_t list, uint32_t size) {
    if (CpuContext* ctx = TryGetCpuContext()) {
        uint32_t gx = 0, dirty = 0;
        if (Memory::TryRead32(ctx->gpr[13] - 0x7DB8u, gx) && gx && Memory::TryRead32(gx + 0x4F4u, dirty) && dirty) {
            func_800FE204(ctx);
        }
    }
    try { Memory::Write8(kDlFifoAddr + kDlWrapFlagOffset, 0); } catch (const Memory::AccessViolation&) {}
    BeginDisplayListRecording(list, size);
}
PPC_NATIVE_OVERRIDE_VOID(801022C8, No100f_GXBeginDisplayList, (uint32_t list, uint32_t size), (list, size));

extern "C" uint32_t No100f_GXEndDisplayList() {
    const uint32_t count = g_dlRecordState.count;
    EndDisplayListRecording();
    uint8_t wrapped = 0;
    try { wrapped = Memory::Read8(kDlFifoAddr + kDlWrapFlagOffset); } catch (const Memory::AccessViolation&) {}
    return wrapped ? 0u : count;  // an overflowed list returns 0, as the SDK does
}
PPC_NATIVE_OVERRIDE(80102390, No100f_GXEndDisplayList, uint32_t, (), ());

static void RaisePeInterruptForBp(uint32_t bpWord) {
    constexpr uint32_t kTokenCallbackAddr = 0x802712B0u;     // TokenCB (r13-0x6FD0)
    constexpr uint32_t kDrawDoneCallbackAddr = 0x802712B4u;  // DrawDoneCB (r13-0x6FCC)
    constexpr uint32_t kDrawDoneFlagAddr = 0x802712B8u;      // DrawDone (r13-0x6FC8)
    constexpr uint32_t kFinishQueueAddr = 0x802712BCu;       // FinishQueue (r13-0x6FC4)
    constexpr uint32_t kOSWakeupThread = 0x801087F4u;
    const uint32_t reg = bpWord >> 24;
    if (reg == 0x47u) g_peHitsToken.fetch_add(1, std::memory_order_relaxed);
    else if (reg == 0x45u) g_peHitsDrawDone.fetch_add(1, std::memory_order_relaxed);
    else if (reg == 0x48u) g_peHitsSync.fetch_add(1, std::memory_order_relaxed);
    else g_peHitsOther.fetch_add(1, std::memory_order_relaxed);
    if (reg == 0x48u) g_gxPeToken = static_cast<uint16_t>(bpWord);  // PE token, read by GXReadDrawSync
    uint32_t cb = 0;
    if (reg == 0x47u) {  // token + interrupt
        g_gxPeToken = static_cast<uint16_t>(bpWord);
        if (Memory::TryRead32(kTokenCallbackAddr, cb) && cb) QueueGxInterruptCallback(cb, bpWord & 0xFFFFu, 0);
    } else if (reg == 0x45u && (bpWord & 2u)) {  // draw done + interrupt (GXSetDrawDone)
        try { Memory::Write8(kDrawDoneFlagAddr, 1); } catch (const Memory::AccessViolation&) {}
        if (Memory::TryRead32(kDrawDoneCallbackAddr, cb) && cb) {
            QueueGxInterruptCallback(cb, 0, 0);
            GxFifo_HLE_NotePresentCompletion();  // Fix 2: as above (unfed in this title; armed for completeness)
        }
        QueueGxInterruptCallback(kOSWakeupThread, kFinishQueueAddr, 0);
    }
}

namespace aurora::gx::fifo {
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);
}

void HleGxState::ResetVertex() {
    currentAttr = NextEnabledAttr(GX_VA_PNMTXIDX - 1);
    currentComp = 0;
}

GXAttr HleGxState::NextEnabledAttr(int startAttr) {
    for (int i = startAttr + 1; i < 26; ++i) {
        if (vtxDesc[i] != GX_NONE) {
            return static_cast<GXAttr>(i);
        }
    }
    return GX_VA_NULL;
}

int HleGxState::GetExpectedCompCount(GXAttr attr, const VtxAttrFmt& fmt) {
    // Attributes with no component layout (matrix indices, XF arrays, GX_VA_NBT)
    // count as one component here: the incremental parser advances one element
    // per raw stream item for them.
    return static_cast<int>(AttrCompCount(attr, fmt, 1u));
}

static void ApplyAuroraVtxStateForRawBegin(GXVtxFmt fmt) {
    // Never publish the GX_VA_NBT row. It has no VAT of its own, and publishing its default
    // (cnt 1, F32) after GX_VA_NRM turned Aurora's normals into NBT (60-byte vertices) while the
    // stream carried 36-byte ones, so every draw of that kind was dropped as truncated.
    PublishAuroraVtxState(fmt, AuroraVtxPublishOptions{/*includeNbt=*/false,
                                                       /*fmtLoopFirst=*/0,
                                                       /*fmtLoopLast=*/25});
}

// There is deliberately no indexed-aurora path here. Immediate-mode indexed
// draws are parsed incrementally, and aurora's indexed-array upload has to
// precede the draw carrying the max-index bounds, which cannot be known without
// buffering the whole primitive. Indexed attributes are therefore expanded into
// the packed direct stream above (GX_INDEX8/16 -> GX_DIRECT).

static uint32_t GetDirectAttrByteSizeForFifo(GXAttr attr, const VtxAttrFmt& fmt) {
    return DirectAttrByteSize(attr, fmt, /*matrixAttrIsOneByte=*/true, /*fallbackComps=*/1u);
}

static bool TryGetRawDirectFifoVertexSize(GXVtxFmt fmt, uint32_t& vertexSize) {
    vertexSize = 0;
    if (fmt >= GX_MAX_VTXFMT) {
        return false;
    }

    for (int attr = 0; attr < 26; ++attr) {
        const GXAttrType type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) {
            continue;
        }
        if (type != GX_DIRECT) {
            return false;
        }

        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const uint32_t attrBytes =
            GetDirectAttrByteSizeForFifo(gxAttr, g_hleGxState.vtxAttrFmt[fmt][attr]);
        if (attrBytes == 0) {
            return false;
        }
        vertexSize += attrBytes;
    }

    return vertexSize != 0;
}

static bool TrySubmitRawDirectFifoDraw(const uint8_t* packet, uint32_t packetBytes, GXPrimitive prim,
                                       GXVtxFmt vtxFmt, uint16_t vtxCount) {
    if (packet == nullptr || packetBytes == 0 || vtxCount == 0) {
        return false;
    }

    EnsureAuroraFrameActive();

    g_hleGxState.currentVtxFmt = vtxFmt;
    g_hleGxState.currentPrim = prim;
    g_hleGxState.vertsRemaining = vtxCount;
    g_hleGxState.inBegin = false;
    g_hleGxState.auroraBeginCalled = false;
    g_hleGxState.ResetVertex();

    ApplyAuroraVtxStateForRawBegin(vtxFmt);
    EnsureDefaultGxAlphaCompare();

    if (!aurora::gx::fifo::submit_raw_draw(prim, vtxFmt, packet + 3, vtxCount, packetBytes - 3u)) {
        return false;
    }
    GXMarkFrameWork();
    return true;
}

static void DecodeColorFromArray(uint32_t addr, GXCompType type, GXCompCnt cnt, GXColor& out) {
    // Gather exactly the bytes the shared decoder consumes. GX_RGBX8 is the one
    // format whose stream footprint (4 bytes) exceeds what is decoded (3), and
    // the pre-dedup code read only those 3 from the guest array - keep it that
    // way so a 3-byte tail at the end of an array cannot start faulting.
    const uint32_t byteCount =
        (type == GX_RGBX8) ? 3u : ColorByteSize(type, cnt);
    uint8_t bytes[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < byteCount && i < 4u; ++i) {
        bytes[i] = Memory::Read8(addr + i);
    }
    DecodeColorBytes(bytes, type, cnt, out);
}

void SubmitIndexedAttribute(GXAttr attr, uint32_t index) {
    const auto& fmt = g_hleGxState.vtxAttrFmt[g_hleGxState.currentVtxFmt][attr];
    const auto& arr = g_hleGxState.vtxArray[attr];
    if (arr.base == 0 || arr.stride == 0) {
        float comps[9]{};
        switch (attr) {
        case GX_VA_CLR0:
        case GX_VA_CLR1:
            if (fmt.cnt == GX_CLR_RGB) {
                GXColor3u8(0, 0, 0);
            } else {
                GXColor4u8(0, 0, 0, 0xFF);
            }
            break;
        case GX_VA_POS:
        case GX_VA_NRM:
        case GX_VA_TEX0:
        case GX_VA_TEX1:
        case GX_VA_TEX2:
        case GX_VA_TEX3:
        case GX_VA_TEX4:
        case GX_VA_TEX5:
        case GX_VA_TEX6:
        case GX_VA_TEX7:
            SubmitAttribute(attr, comps, fmt);
            break;
        default:
            break;
        }
        return;
    }
    const uint32_t baseAddr = arr.base + index * arr.stride;
    float comps[9]{};
    u32 rawComps[9]{};
    switch (attr) {
    case GX_VA_POS: {
        const int count = static_cast<int>(AttrCompCount(GX_VA_POS, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    case GX_VA_NRM: {
        const int count = static_cast<int>(AttrCompCount(GX_VA_NRM, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    case GX_VA_CLR0:
    case GX_VA_CLR1: {
        GXColor color{};
        DecodeColorFromArray(baseAddr, fmt.type, fmt.cnt, color);
        if (fmt.cnt == GX_CLR_RGB) {
            GXColor3u8(color.r, color.g, color.b);
        } else {
            GXColor4u8(color.r, color.g, color.b, color.a);
        }
        break;
    }
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7: {
        // Every GX_VA_TEXn shares one component layout, so the constant here is
        // exact for whichever of them `attr` is.
        const int count = static_cast<int>(AttrCompCount(GX_VA_TEX0, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    default:
        break;
    }
}

// Mirrors the raw-integer fast paths in SubmitAttribute below: for those
// (attr, type) pairs the float component buffer is never read, so the direct
// FIFO path can skip building it.
static bool SubmitAttributeReadsFloatComps(GXAttr attr, const VtxAttrFmt& fmt) {
    switch (attr) {
    case GX_VA_POS:
    case GX_VA_NRM:
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7:
        break;
    default:
        return true;
    }
    switch (fmt.type) {
    case GX_U8:
    case GX_S8:
    case GX_U16:
    case GX_S16:
        return false;
    default:
        return true;
    }
}

void SubmitAttribute(GXAttr attr, float* comps, const VtxAttrFmt& fmt, const u32* rawComps) {
    if (attr == GX_VA_POS) {
        float bb[3] = {comps[0], comps[1], comps[2]};
        const int n = fmt.cnt == GX_POS_XY ? 2 : 3;
        if (rawComps && fmt.type != GX_F32) {
            for (int i = 0; i < n; ++i) bb[i] = ConvertCompToFloat(rawComps[i], fmt.type, fmt.frac);
        }
        GxBBoxPosition(bb, n);
    }
    auto r8 = [rawComps](int idx) { return static_cast<u8>(rawComps[idx]); };
    auto rs8 = [rawComps](int idx) { return static_cast<s8>(rawComps[idx]); };
    auto r16 = [rawComps](int idx) { return static_cast<u16>(rawComps[idx]); };
    auto rs16 = [rawComps](int idx) { return static_cast<s16>(rawComps[idx]); };

    switch (attr) {
    case GX_VA_POS:
        if (rawComps) {
            if (fmt.cnt == GX_POS_XY) {
                switch (fmt.type) {
                case GX_U8: GXPosition2u8(r8(0), r8(1)); return;
                case GX_S8: GXPosition2s8(rs8(0), rs8(1)); return;
                case GX_U16: GXPosition2u16(r16(0), r16(1)); return;
                case GX_S16: GXPosition2s16(rs16(0), rs16(1)); return;
                default: break;
                }
            } else {
                switch (fmt.type) {
                case GX_U8: GXPosition3u8(r8(0), r8(1), r8(2)); return;
                case GX_S8: GXPosition3s8(rs8(0), rs8(1), rs8(2)); return;
                case GX_U16: GXPosition3u16(r16(0), r16(1), r16(2)); return;
                case GX_S16: GXPosition3s16(rs16(0), rs16(1), rs16(2)); return;
                default: break;
                }
            }
        }
        if (fmt.cnt == GX_POS_XY) {
            GXPosition2f32(comps[0], comps[1]);
        } else {
            GXPosition3f32(comps[0], comps[1], comps[2]);
        }
        break;
    case GX_VA_NRM:
        if (rawComps) {
            const bool nbt = fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3;
            const int groups = nbt ? 3 : 1;
            switch (fmt.type) {
            case GX_U8:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3u8(r8(i * 3), r8(i * 3 + 1), r8(i * 3 + 2));
                }
                return;
            case GX_S8:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3s8(rs8(i * 3), rs8(i * 3 + 1), rs8(i * 3 + 2));
                }
                return;
            case GX_U16:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3u16(r16(i * 3), r16(i * 3 + 1), r16(i * 3 + 2));
                }
                return;
            case GX_S16:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3s16(rs16(i * 3), rs16(i * 3 + 1), rs16(i * 3 + 2));
                }
                return;
            default: break;
            }
        }
        if (fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3) {
            for (int i = 0; i < 3; ++i) {
                GXNormal3f32(comps[i * 3], comps[i * 3 + 1], comps[i * 3 + 2]);
            }
        } else {
            GXNormal3f32(comps[0], comps[1], comps[2]);
        }
        break;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        if (fmt.cnt == GX_CLR_RGB) {
            GXColor3u8(static_cast<u8>(comps[0]), static_cast<u8>(comps[1]),
                       static_cast<u8>(comps[2]));
        } else {
            GXColor4u8(static_cast<u8>(comps[0]), static_cast<u8>(comps[1]),
                       static_cast<u8>(comps[2]), static_cast<u8>(comps[3]));
        }
        break;
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7:
        if (rawComps) {
            if (fmt.cnt == GX_TEX_S) {
                switch (fmt.type) {
                case GX_U8: GXTexCoord1u8(r8(0)); return;
                case GX_S8: GXTexCoord1s8(rs8(0)); return;
                case GX_U16: GXTexCoord1u16(r16(0)); return;
                case GX_S16: GXTexCoord1s16(rs16(0)); return;
                default: break;
                }
            } else {
                switch (fmt.type) {
                case GX_U8: GXTexCoord2u8(r8(0), r8(1)); return;
                case GX_S8: GXTexCoord2s8(rs8(0), rs8(1)); return;
                case GX_U16: GXTexCoord2u16(r16(0), r16(1)); return;
                case GX_S16: GXTexCoord2s16(rs16(0), rs16(1)); return;
                default: break;
                }
            }
        }
        if (fmt.cnt == GX_TEX_S) {
            GXTexCoord1f32(comps[0]);
        } else {
            GXTexCoord2f32(comps[0], comps[1]);
        }
        break;
    default: break;
    }
}

// `val` is a raw big-endian bit pattern: the FIFO stream is type-agnostic, and
// the float entry point converts before it gets here.
// NO100F: RenderWare's immediate-mode renderer sometimes issues GXBegin with a vertex count of 0 and then streams
// vertices anyway (seen when a cutscene is skipped). Parsed literally, those floats become commands and the stream
// desyncs into garbage draws/calls. After a zero-count draw the parser drops raw writes until the next 1-byte write of
// a valid command byte: native GX code starts every command with a single-byte store, vertex data comes in wider ones.
static bool g_dropRawVertices = false;
struct PipeWrite { u32 val; uint32_t size; uint32_t active; };
constexpr uint32_t kPipeRing = 4096;
static PipeWrite g_pipeRing[kPipeRing];
static uint32_t g_pipeRingPos = 0;
// Walks back from the newest write: each run of 1-/2-byte writes (command bytes, draw headers, counts) is printed with
// the number of 4-byte writes that followed it, so a draw header's count can be compared with the vertex data sent.
static void DumpPipeRing() {
    RT_LOGF(RT_TAG_GX, "[pipe] newest first (size:value@guest, then +N wide writes after it):");
    uint32_t wide = 0, shown = 0;
    for (uint32_t i = 1; i <= kPipeRing && shown < 24; ++i) {
        const PipeWrite& w = g_pipeRing[(g_pipeRingPos + kPipeRing - i) % kPipeRing];
        if (w.size == 0) break;
        if (w.size >= 4) { ++wide; continue; }
        RT_LOGF(RT_TAG_GX, " [%u:%0*X@%08X +%u]", w.size, int(w.size * 2), w.val, w.active, wide);
        wide = 0; ++shown;
    }
    RT_LOGF(RT_TAG_GX, "\n");
}
static bool IsCommandByte(u32 v) {
    return v == 0x00 || v == 0x08 || v == 0x10 || v == 0x20 || v == 0x28 || v == 0x30 || v == 0x38 || v == 0x40 ||
           v == 0x48 || v == 0x61 || (v >= 0x80 && v <= 0xBF);
}

// Gather-pipe traffic from the game thread since start, for hle/vi.cpp's per-600-frame line: every
// translated store to 0xCC008000 is one HleFifoWrite (memory.cpp Write*Slow -> gx_stubs.cpp), so the
// call count is the per-word dispatch cost the route profile showed (~14% of the game thread).
// Game thread only: plain counters.
static uint64_t g_fifoWriteCalls = 0, g_fifoWriteBytes = 0, g_fifoDlWriteCalls = 0, g_fifoBurstCalls = 0,
                g_fifoBurstBytes = 0;
void GxFifoWriteTotals(uint64_t* calls, uint64_t* bytes, uint64_t* dlCalls, uint64_t* burstCalls,
                       uint64_t* burstBytes) {
    *calls = g_fifoWriteCalls;
    *bytes = g_fifoWriteBytes;
    *dlCalls = g_fifoDlWriteCalls;
    *burstCalls = g_fifoBurstCalls;
    *burstBytes = g_fifoBurstBytes;
}

// The raw-write ring above costs a store and an execution-address read on every pipe word; it is
// only read by DumpPipeRing (zero-count draws). On with NO100F_PIPE_RING (dk_diag "pipering").
static bool PipeRingEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("NO100F_PIPE_RING");
        return e != nullptr && e[0] != '0';
    }();
    return on;
}

// fifofast (NO100F_FIFO_FAST; dk_diag "fifofast", "fifofast=2"): once a raw draw's header is in and
// its whole packet size is known, later pipe words are only appended until the packet is complete,
// instead of re-running the parse loop (header read, vertex-size lookup) on every word. The packet
// bytes and the moment it is submitted are the same as the slow path's: the parse loop runs again
// exactly when the buffered bytes reach the size the slow path would wait for. The pending state is
// trusted only while the buffer still starts with the same 3 header bytes, and any reset drops it.
// Mode 2 also recomputes the packet size on every fast-path word, as the slow path would, and logs
// the first time they differ (then falls back to the slow path for that packet).
static int FifoFastMode() {
    static const int mode = [] {
        const char* e = std::getenv("NO100F_FIFO_FAST");
        return e == nullptr ? 0 : std::atoi(e);
    }();
    return mode;
}
static uint32_t g_pendingRawPacketBytes = 0;  // 0: none
static uint8_t g_pendingRawHeader[3] = {};
static uint64_t g_fifoFastWords = 0, g_fifoFastMismatches = 0;
void GxFifoFastTotals(uint64_t* fastWords, uint64_t* mismatches) {
    *fastWords = g_fifoFastWords;
    *mismatches = g_fifoFastMismatches;
}

// L2 write-side counters (N8 L2 counter build): HleFifoWrite time split +
// single-word run-length histogram. Game thread only: plain counters, the
// same idiom as g_fifoWriteCalls above. Gated on the nativescene dk_diag
// key (No100fNativeSceneEnabled), so the toggle set reads zero. The time
// split is SAMPLED 1 in 64 calls: a clock read costs ~38 ns (clock_cal)
// and the route runs ~39k HleFifoWrite calls per frame, so timing every
// call would cost ~6 ms/frame of instrument; the sampled buckets are
// scaled by total/sampled at the print site in vi.cpp. The histogram
// counts every direct call (no clock reads).
static uint64_t s_l2CallCount = 0;     // HleFifoWrite calls seen while nativescene is on
static uint64_t s_l2SampledCalls = 0;  // timed calls (1 in 64)
static uint64_t s_l2DispatchNanos = 0; // entry to the store (sampled)
static uint64_t s_l2StoreNanos = 0;    // the ring append itself (sampled)
static uint64_t s_l2WakeNanos = 0;     // after the store: fifofast check + parse loop (sampled)
static uint64_t s_l2RunLen = 0;        // current run of consecutive direct single-word calls
static uint64_t s_l2RunWords = 0;      // direct single-word calls that joined a run
static uint64_t s_l2Runs1 = 0, s_l2Runs2_4 = 0, s_l2Runs5_16 = 0, s_l2Runs17 = 0;
static bool s_l2InChunkedBurst = false;  // HleFifoWriteBurstChunked's inner calls are burst words
void GxL2Totals(uint64_t* sampledCalls, uint64_t* dispatchNanos, uint64_t* storeNanos, uint64_t* wakeNanos,
                uint64_t* runWords, uint64_t* runs1, uint64_t* runs2_4, uint64_t* runs5_16,
                uint64_t* runs17) {
    *sampledCalls = s_l2SampledCalls;
    *dispatchNanos = s_l2DispatchNanos;
    *storeNanos = s_l2StoreNanos;
    *wakeNanos = s_l2WakeNanos;
    *runWords = s_l2RunWords;
    *runs1 = s_l2Runs1;
    *runs2_4 = s_l2Runs2_4;
    *runs5_16 = s_l2Runs5_16;
    *runs17 = s_l2Runs17;
}
// Bucket the current single-word run and reset it. A run ends at a burst
// (GX_HLE_FIFO_WriteBurst entry) or a display-list write; a run still open
// at a window boundary is counted when it next flushes.
static void l2_flush_run() {
    if (s_l2RunLen == 0) return;
    if (s_l2RunLen == 1) ++s_l2Runs1;
    else if (s_l2RunLen <= 4) ++s_l2Runs2_4;
    else if (s_l2RunLen <= 16) ++s_l2Runs5_16;
    else ++s_l2Runs17;
    s_l2RunLen = 0;
}
// RAII: mark HleFifoWrite calls made by HleFifoWriteBurstChunked as burst
// words (they must not extend the single-word run); restores the previous
// value so a nested burst cannot clear the flag early.
struct L2ChunkedBurstScope {
    bool prev;
    L2ChunkedBurstScope() : prev(s_l2InChunkedBurst) { s_l2InChunkedBurst = true; }
    ~L2ChunkedBurstScope() { s_l2InChunkedBurst = prev; }
};
// L2 time-split guard: records t3 at every HleFifoWrite exit and books the
// sampled split. t1/t2 are set by the body just before/after the store; a
// return before t1 (display-list recording) books the whole call as dispatch.
struct L2WriteTimer {
    bool sample;
    std::chrono::steady_clock::time_point t0;
    std::chrono::steady_clock::time_point& t1;
    std::chrono::steady_clock::time_point& t2;
    ~L2WriteTimer() {
        if (!sample) return;
        const auto t3 = std::chrono::steady_clock::now();
        const auto ns = [](std::chrono::steady_clock::time_point a,
                           std::chrono::steady_clock::time_point b) {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        };
        if (t1 == std::chrono::steady_clock::time_point{}) {
            s_l2DispatchNanos += ns(t0, t3);
            return;
        }
        s_l2DispatchNanos += ns(t0, t1);
        s_l2StoreNanos += ns(t1, t2);
        s_l2WakeNanos += ns(t2, t3);
    }
};

// phasetime (PHASETIME design, Nov 19, 2026): the first GX packet the parser completes after
// each present, as a steady-clock timestamp (nanos since epoch; 0 = none this interval).
// Written here (the parser's consumeBytes choke: every parse-loop completion passes through
// it; the vertex-by-vertex path below does not), read + cleared by hle/vi.cpp's present hook.
static std::atomic<uint64_t> g_phaseFirstPacketNanos{0};
static bool PhaseTimeArmed() {
    static const bool armed = std::getenv("SCOOBY_PHASETIME") != nullptr;
    return armed;
}
uint64_t PhaseTimeFirstPacketNanos() { return g_phaseFirstPacketNanos.load(std::memory_order_relaxed); }
void PhaseTimeClearFirstPacket() { g_phaseFirstPacketNanos.store(0, std::memory_order_relaxed); }

void HleFifoWrite(u32 val, uint32_t sizeBytes) {
    // L2: sample the time split 1 in 64 calls (see the note above GxL2Totals).
    const bool l2On = No100fNativeSceneEnabled();
    const bool l2Sample = l2On && ((++s_l2CallCount & 63) == 0);
    if (l2Sample) ++s_l2SampledCalls;
    const auto l2T0 = l2Sample ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    std::chrono::steady_clock::time_point l2T1{}, l2T2{};
    L2WriteTimer l2Timer{l2Sample, l2T0, l2T1, l2T2};
    ++g_fifoWriteCalls;
    g_fifoWriteBytes += sizeBytes == 0 ? 4 : sizeBytes;
    const bool recordOnly = IsDisplayListActive();
    if (recordOnly) {
        ++g_fifoDlWriteCalls;
        WriteDisplayListData(val, sizeBytes);
        l2_flush_run();  // a display-list write breaks the raw single-word run
        return;
    }
    // L2: a direct raw single-word call extends the run (burst words do not).
    if (l2On && !s_l2InChunkedBurst) {
        ++s_l2RunLen;
        ++s_l2RunWords;
    }
    if (PipeRingEnabled()) {   // NO100F diagnostic: the last raw pipe writes, dumped when the parser meets a zero-count draw.
        g_pipeRing[g_pipeRingPos % kPipeRing] = {val, sizeBytes, RecompMod::CurrentTranslatedExecutionAddress()};
        ++g_pipeRingPos;
    }
    if (g_dropRawVertices) {
        if (sizeBytes != 1 || !IsCommandByte(val)) return;
        g_dropRawVertices = false;
    }

    auto resetFifoBuffer = [&]() {
        g_hleGxState.fifoReadOffset = 0;
        g_hleGxState.fifoByteCount = 0;
    };

    auto compactFifoBuffer = [&]() {
        if (g_hleGxState.fifoByteCount == 0) {
            g_hleGxState.fifoReadOffset = 0;
            return;
        }
        if (g_hleGxState.fifoReadOffset == 0) {
            return;
        }
        std::memmove(g_hleGxState.fifoBytes.data(),
                     g_hleGxState.fifoBytes.data() + g_hleGxState.fifoReadOffset,
                     g_hleGxState.fifoByteCount);
        g_hleGxState.fifoReadOffset = 0;
    };

    auto fifoData = [&]() -> uint8_t* {
        return g_hleGxState.fifoBytes.data() + g_hleGxState.fifoReadOffset;
    };

    auto pushBytes = [&](u32 value, uint32_t count) {
        if (count == 0) return;
        if (count > 4) count = 4;
        if (g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount + count > g_hleGxState.fifoBytes.size()) {
            compactFifoBuffer();
            if (g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount + count > g_hleGxState.fifoBytes.size()) {
                // Grow rather than drop: a pending draw packet larger than the buffer could otherwise never complete.
                if (g_hleGxState.fifoBytes.size() < 16u * 1024 * 1024) {
                    g_hleGxState.fifoBytes.resize(g_hleGxState.fifoBytes.size() * 2);
                } else {
                    resetFifoBuffer();
                }
            }
        }
        const size_t writeOffset = g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount;
        switch (count) {
        case 4:
            BigEndian::Write32(g_hleGxState.fifoBytes.data() + writeOffset, value);
            break;
        case 2:
            BigEndian::Write16(g_hleGxState.fifoBytes.data() + writeOffset,
                               static_cast<uint16_t>(value));
            break;
        default:
            g_hleGxState.fifoBytes[writeOffset] = static_cast<uint8_t>(value & 0xFF);
            break;
        }
        g_hleGxState.fifoByteCount += count;
    };

    bool inParseLoop = false;  // phasetime: true only around the parse loop below, never in the vertex loop
    auto consumeBytes = [&](uint32_t count, u32& out) -> bool {
        if (count == 0 || g_hleGxState.fifoByteCount < count) return false;
        u32 value = 0;
        const uint32_t foldCount = (count > 4) ? 4 : count;
        uint8_t* data = fifoData();
        for (uint32_t i = 0; i < foldCount; ++i) {
            value = (value << 8) | data[i];
        }
        g_hleGxState.fifoReadOffset += count;
        g_hleGxState.fifoByteCount -= count;
        if (g_hleGxState.fifoByteCount == 0) {
            g_hleGxState.fifoReadOffset = 0;
        }
        out = value;
        // phasetime: stamp the first packet of the parse loop (per present interval).
        if (inParseLoop && PhaseTimeArmed() &&
            g_phaseFirstPacketNanos.load(std::memory_order_relaxed) == 0) {
            g_phaseFirstPacketNanos.store(
                static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()),
                std::memory_order_relaxed);
        }
        return true;
    };

    if (l2Sample) l2T1 = std::chrono::steady_clock::now();
    pushBytes(val, sizeBytes == 0 ? 4 : sizeBytes);
    if (l2Sample) l2T2 = std::chrono::steady_clock::now();

    // fifofast: a raw draw packet is still filling; nothing to parse until it is whole.
    if (g_pendingRawPacketBytes != 0) {
        const uint8_t* head = fifoData();
        const bool same = !g_hleGxState.inBegin && g_hleGxState.fifoByteCount >= 3 &&
                          head[0] == g_pendingRawHeader[0] && head[1] == g_pendingRawHeader[1] &&
                          head[2] == g_pendingRawHeader[2];
        if (!same) {
            g_pendingRawPacketBytes = 0;  // the buffer moved on under us: the slow path decides
        } else if (g_hleGxState.fifoByteCount < g_pendingRawPacketBytes) {
            if (FifoFastMode() == 2) {
                // what the slow path would wait for now
                uint32_t rawVertexSize = 0;
                const GXVtxFmt fmt = static_cast<GXVtxFmt>(head[0] & GX_VAT_MASK_CMD);
                const uint32_t slowBytes = TryGetRawDirectFifoVertexSize(fmt, rawVertexSize)
                                               ? 3u + static_cast<uint32_t>(ReadBE16(head + 1)) * rawVertexSize
                                               : 0u;
                if (slowBytes != g_pendingRawPacketBytes) {
                    if (g_fifoFastMismatches++ == 0) {
                        RT_LOGF(RT_TAG_GX, "[gxfifo] fifofast mismatch: cached packet %u bytes, slow path now %u (cmd 0x%02X)\n",
                                g_pendingRawPacketBytes, slowBytes, head[0]);
                    }
                    g_pendingRawPacketBytes = 0;
                }
            }
            if (g_pendingRawPacketBytes != 0) {
                ++g_fifoFastWords;
                return;  // the slow path would break at the same size check
            }
        } else {
            g_pendingRawPacketBytes = 0;  // whole now: parse it below
        }
    }

    // Parse raw FIFO command packets written directly to the gather pipe
    // (e.g. NW4R/G3D paths that do not call the GXBegin wrapper function).
    inParseLoop = true;
    while (!g_hleGxState.inBegin) {
        if (g_hleGxState.fifoByteCount < 1) {
            break;
        }

        uint8_t* data = fifoData();
        const uint8_t cmd = data[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
        u32 sink = 0;

        if (cmd == GX_NOP_CMD || opcode == GX_CMD_INVL_VC_CMD) {
            if (!consumeBytes(1, sink)) break;
            continue;
        }

        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint32_t bpWord = ReadBE32(data + 1);
            GXApplyBPReg(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
            RaisePeInterruptForBp(bpWord);
            if (!consumeBytes(5, sink)) break;
            continue;
        }

        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 6) break;
            const uint8_t reg = data[1];
            const uint32_t cpValue = ReadBE32(data + 2);
            GxCpDecode::ApplyCpRegWrite(reg, cpValue);
            if (!consumeBytes(6, sink)) break;
            continue;
        }

        if (opcode == GX_LOAD_XF_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint16_t countWords = ReadBE16(data + 1);
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(countWords) + 1u) * 4u;
            if (g_hleGxState.fifoByteCount < packetBytes) break;
            GXCallDisplayList(data, packetBytes);
            GXMarkFrameWork();
            if (!consumeBytes(packetBytes, sink)) break;
            continue;
        }

        if (opcode >= GX_LOAD_INDX_A_CMD && opcode <= GX_LOAD_INDX_D_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint32_t xfValue = ReadBE32(data + 1);
            ApplyIndexedXfArrayForPacket(cmd, xfValue);
            GXCallDisplayList(data, 5);
            GXMarkFrameWork();
            if (!consumeBytes(5, sink)) break;
            continue;
        }

        if (opcode == GX_CMD_CALL_DL_CMD) {
            if (g_hleGxState.fifoByteCount < 9) break;
            const uint32_t listAddr = ReadBE32(data + 1);
            const uint32_t listSize = ReadBE32(data + 5);
            if (!consumeBytes(9, sink)) break;
            if (listAddr != 0 && listSize > 0) {
                GX__CallDisplayList_80172f64(listAddr, listSize);
            }
            continue;
        }

        if (IsDrawOpcode(opcode)) {
            if (g_hleGxState.fifoByteCount < 3) break;
            const uint16_t vtxCount = ReadBE16(data + 1);
            const GXVtxFmt vtxFmt = static_cast<GXVtxFmt>(cmd & GX_VAT_MASK_CMD);
            const GXPrimitive prim = OpcodeToGXPrimitive(cmd);
            if (vtxCount == 0) {
                static int logged = 0;
                if (logged++ < 20) {
                    RT_LOGF(RT_TAG_GX, "[gxfifo] draw 0x%02X with 0 vertices: dropping its vertex stream (active 0x%08X)\n",
                            cmd, RecompMod::CurrentTranslatedExecutionAddress());
                    // NO100F diagnostic: RenderWare's Im3D stash (*(r13-0x6B18) = *(0x80271768)) at the moment of the
                    // zero-count draw: +0 u16 numVerts, +4 verts, +0x30 prim, +0x34 indices, +0x38 count, +0x2C = T+32 pipe.
                    if (logged <= 3) DumpPipeRing();
                    const uint32_t stash = Memory::Read32(0x80271768u);
                    if (stash >= 0x80000000u && stash < 0x81800000u) {
                        RT_LOGF(RT_TAG_GX, "[gxfifo]   im3d stash %08X:", stash);
                        for (uint32_t i = 0; i < 16; ++i) RT_LOGF(RT_TAG_GX, " %08X", Memory::Read32(stash + 4 * i));
                        RT_LOGF(RT_TAG_GX, "\n");
                    }
                }
                if (!consumeBytes(3, sink)) break;
                g_hleGxState.fifoReadOffset = 0;  // anything already buffered behind it is that vertex data
                g_hleGxState.fifoByteCount = 0;
                g_dropRawVertices = true;
                break;
            }
            if (g_hleGxState.vtxDesc[GX_VA_POS] == GX_NONE) {
                // No position: hardware draws nothing useful and Aurora aborts building the shader. Seen when the
                // stream is misparsed or a draw precedes its vertex descriptor; drop the command byte.
                static int logged = 0;
                if (logged++ < 20) {
                    RT_LOGF(RT_TAG_GX, "[gxfifo] draw 0x%02X n=%u without GX_VA_POS dropped (active 0x%08X)\n", cmd,
                            vtxCount, RecompMod::CurrentTranslatedExecutionAddress());
                }
                if (!consumeBytes(1, sink)) break;
                continue;
            }
            uint32_t rawVertexSize = 0;
            if (TryGetRawDirectFifoVertexSize(vtxFmt, rawVertexSize)) {
                const uint32_t packetBytes = 3u + static_cast<uint32_t>(vtxCount) * rawVertexSize;
                if (g_hleGxState.fifoByteCount < packetBytes) {
                    if (FifoFastMode() != 0) {  // fifofast: wait for the rest without parsing
                        g_pendingRawPacketBytes = packetBytes;
                        std::memcpy(g_pendingRawHeader, data, 3);
                    }
                    break;
                }
                if (TrySubmitRawDirectFifoDraw(data, packetBytes, prim, vtxFmt, vtxCount)) {
                    if (!consumeBytes(packetBytes, sink)) break;
                    continue;
                }
            }
            if (!consumeBytes(3, sink)) break;

            g_hleGxState.currentVtxFmt = vtxFmt;
            g_hleGxState.currentPrim = prim;
            g_hleGxState.vertsRemaining = vtxCount;
            g_hleGxState.inBegin = true;
            g_hleGxState.auroraBeginCalled = false;
            g_hleGxState.ResetVertex();

            break;
        }

        // Unknown FIFO command byte outside a begin packet; discard it so stream parsing can recover.
        if (!consumeBytes(1, sink)) break;
    }
    inParseLoop = false;

    if (!g_hleGxState.inBegin) {
        return;
    }

    if (!recordOnly && !g_hleGxState.auroraBeginCalled) {
        EnsureAuroraFrameActive();
        ApplyAuroraVtxStateForRawBegin(g_hleGxState.currentVtxFmt);
        EnsureDefaultGxAlphaCompare();
        GXBegin(g_hleGxState.currentPrim, g_hleGxState.currentVtxFmt, static_cast<u16>(g_hleGxState.vertsRemaining));
        g_hleGxState.auroraBeginCalled = true;
        GXMarkFrameWork();
    }

    auto finishVertexIfNeeded = [&](GXAttr prevAttr) {
        g_hleGxState.currentAttr = g_hleGxState.NextEnabledAttr(prevAttr);
        g_hleGxState.currentComp = 0;
        if (g_hleGxState.currentAttr == GX_VA_NULL) {
            g_hleGxState.ResetVertex();
            if (g_hleGxState.vertsRemaining > 0) {
                g_hleGxState.vertsRemaining--;
                if (g_hleGxState.vertsRemaining == 0) {
                    g_hleGxState.inBegin = false;
                    g_hleGxState.auroraBeginCalled = false;
                    if (!recordOnly) {
                        GXEnd();
                    }
                }
            }
        }
    };

    while (true) {
        if (!g_hleGxState.inBegin) {
            break;
        }
        GXAttr attr = g_hleGxState.currentAttr;
        if (attr == GX_VA_NULL) {
            resetFifoBuffer();
            return;
        }

        GXAttrType inputType = g_hleGxState.vtxDesc[attr];
        const VtxAttrFmt& fmt = g_hleGxState.vtxAttrFmt[g_hleGxState.currentVtxFmt][attr];

        if (IsMatrixIndexAttr(attr)) {
            if (g_hleGxState.fifoByteCount < 1) break;
            u32 raw = 0;
            if (!consumeBytes(1, raw)) break;
            if (!recordOnly) {
                if (attr == GX_VA_PNMTXIDX) GxBBoxVertexMatrix(raw);
                GXMatrixIndex1u8(attr, static_cast<u8>(raw));
            }
            finishVertexIfNeeded(attr);
            continue;
        }

        if (inputType == GX_DIRECT) {
            if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
                const uint32_t colorSize = ColorByteSize(fmt.type, fmt.cnt);

                if (g_hleGxState.fifoByteCount < colorSize) break;

                uint8_t colorBytes[4] = {0, 0, 0, 0};
                uint8_t* data = fifoData();
                for (uint32_t i = 0; i < colorSize && i < 4; ++i) {
                    colorBytes[i] = data[i];
                }
                g_hleGxState.fifoReadOffset += colorSize;
                g_hleGxState.fifoByteCount -= colorSize;
                if (g_hleGxState.fifoByteCount == 0) {
                    g_hleGxState.fifoReadOffset = 0;
                }

                GXColor color{};
                DecodeColorBytes(colorBytes, fmt.type, fmt.cnt, color);

                if (!recordOnly) {
                    if (fmt.cnt == GX_CLR_RGB) {
                        GXColor3u8(color.r, color.g, color.b);
                    } else {
                        GXColor4u8(color.r, color.g, color.b, color.a);
                    }
                }
                finishVertexIfNeeded(attr);
                continue;
            }
            
            const int compSize = GetCompSizeBytes(fmt.type);
            if (compSize <= 0 || g_hleGxState.fifoByteCount < static_cast<size_t>(compSize)) break;
            u32 raw = 0;
            if (!consumeBytes(static_cast<uint32_t>(compSize), raw)) break;
            constexpr int kMaxComps = 9;
            if (g_hleGxState.currentComp < kMaxComps) {
                if (SubmitAttributeReadsFloatComps(attr, fmt)) {
                    g_hleGxState.compBuffer[g_hleGxState.currentComp] =
                        ConvertCompToFloat(raw, fmt.type, fmt.frac);
                }
                g_hleGxState.rawCompBuffer[g_hleGxState.currentComp] = raw;
            }
            g_hleGxState.currentComp++;
            const int expected = g_hleGxState.GetExpectedCompCount(attr, fmt);
            if (g_hleGxState.currentComp >= expected) {
                if (!recordOnly) {
                    SubmitAttribute(attr, g_hleGxState.compBuffer, fmt, g_hleGxState.rawCompBuffer);
                }
                finishVertexIfNeeded(attr);
            }
            continue;
        }

        if (inputType == GX_INDEX8 || inputType == GX_INDEX16) {
            const uint32_t idxSize = (inputType == GX_INDEX8) ? 1u : 2u;
            const uint32_t indexCount = (attr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
            if (g_hleGxState.fifoByteCount < idxSize * indexCount) break;
            u32 raw[3]{};
            for (uint32_t i = 0; i < indexCount; ++i) {
                if (!consumeBytes(idxSize, raw[i])) break;
            }
            if (!recordOnly) {
                if (attr == GX_VA_NRM && indexCount == 3u) {
                    SubmitIndexedNormalNBT3(raw, fmt);
                } else {
                    SubmitIndexedAttribute(attr, raw[0]);
                }
            }
            finishVertexIfNeeded(attr);
            continue;
        }

        g_hleGxState.currentAttr = g_hleGxState.NextEnabledAttr(attr);
        g_hleGxState.currentComp = 0;
        if (g_hleGxState.currentAttr == GX_VA_NULL) {
            g_hleGxState.ResetVertex();
        }
        break;
    }
}

// Must behave exactly like GX_HLE_FIFO_Write32/16/8 run byte by byte. The ring is not
// bulk-appended ahead of a single parse because the parser can re-enter GX HLE (nested display
// lists, XF/draw calls), which would misorder that work relative to the stream.
static void HleFifoWriteBurstChunked(const uint8_t* data, uint32_t sizeBytes) {
    L2ChunkedBurstScope l2Chunked;  // inner HleFifoWrite calls are burst words, not run members
    uint32_t offset = 0;
    for (; offset + 4u <= sizeBytes; offset += 4u) {
        HleFifoWrite(ReadBE32(data + offset), 4);
    }
    if (offset + 2u <= sizeBytes) {
        HleFifoWrite(static_cast<u32>(ReadBE16(data + offset)), 2);
        offset += 2u;
    }
    if (offset < sizeBytes) {
        HleFifoWrite(static_cast<u32>(data[offset]), 1);
    }
}

// Applies complete register-load packets straight to the parser's own entry points, skipping the
// serialize/ring/re-parse round trip; returns bytes consumed, caller hands the rest to
// HleFifoWriteBurstChunked. Safe (per gd_fifo_hle.cpp's GdCanApplyDirect) only on a packet
// boundary with nothing buffered, outside a GXBegin packet and outside display-list recording;
// any other opcode or a truncated packet just stops the walk.
static uint32_t ApplyFifoPacketsDirect(const uint8_t* data, uint32_t sizeBytes) {
    uint32_t offset = 0;

    while (offset < sizeBytes) {
        // Re-tested per packet, not once per burst: nothing currently re-enters GX HLE mid-walk,
        // but if it ever does, breaking here just hands the remainder to the ring.
        if (IsDisplayListActive() || g_hleGxState.inBegin || g_hleGxState.fifoByteCount != 0) {
            break;
        }

        const uint8_t* packet = data + offset;
        const uint32_t avail = sizeBytes - offset;
        const uint8_t cmd = packet[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;

        if (cmd == GX_NOP_CMD) {
            offset += 1u;
            continue;
        }

        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (avail < 5u) break;
            const uint32_t bpWord = ReadBE32(packet + 1);
            GXApplyBPReg(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
            RaisePeInterruptForBp(bpWord);
            offset += 5u;
            continue;
        }

        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (avail < 6u) break;
            const uint8_t reg = packet[1];
            const uint32_t cpValue = ReadBE32(packet + 2);
            // Same function the parser calls, so the CP registers it does not
            // decode (0x30/0x40 among them) are dropped here identically.
            GxCpDecode::ApplyCpRegWrite(reg, cpValue);
            offset += 6u;
            continue;
        }

        if (opcode == GX_LOAD_XF_REG_CMD) {
            if (avail < 5u) break;
            const uint16_t countWords = ReadBE16(packet + 1);
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(countWords) + 1u) * 4u;
            if (avail < packetBytes) break;
            GXCallDisplayList(packet, packetBytes);
            GXMarkFrameWork();
            offset += packetBytes;
            continue;
        }

        break;
    }

    return offset;
}

// Display-list recording just copies bytes into the guest list buffer and advances the shadow
// cursor, so a burst is one logical append as long as it doesn't cross the list end; a burst that
// would wrap falls back to the per-write path instead. Returns false to signal that fallback.
static bool WriteDisplayListBurst(const uint8_t* data, uint32_t sizeBytes) {
    GxDisplayListState& dl = g_dlRecordState;
    if (!dl.active || dl.base == 0 || dl.size == 0 || dl.writePtr == 0) {
        // Same guard WriteDisplayListData applies; let the per-write path
        // reproduce its (silent) drop.
        return false;
    }

    const uint32_t end = dl.base + dl.size;
    if (dl.writePtr > end || (end - dl.writePtr) < sizeBytes) {
        return false;
    }

    // Guest-visible bytes are written through the ordinary store path, so
    // unaligned cursors, MMIO policy and executable-write guards all behave as
    // they do for a single store. The cursor and count advance per element so a
    // failed write in the middle leaves exactly the completed prefix recorded.
    try {
        uint32_t offset = 0;
        for (; offset + 4u <= sizeBytes; offset += 4u) {
            Memory::Write32(dl.writePtr, ReadBE32(data + offset));
            dl.writePtr += 4u;
            dl.count += 4u;
        }
        if (offset + 2u <= sizeBytes) {
            Memory::Write16(dl.writePtr, ReadBE16(data + offset));
            dl.writePtr += 2u;
            dl.count += 2u;
            offset += 2u;
        }
        if (offset < sizeBytes) {
            Memory::Write8(dl.writePtr, data[offset]);
            dl.writePtr += 1u;
            dl.count += 1u;
        }
    } catch (const Memory::AccessViolation&) {
    }
    return true;
}

extern "C" void GX_HLE_FIFO_WriteBurst(const uint8_t* data, uint32_t sizeBytes) {
    if (data == nullptr || sizeBytes == 0) {
        return;
    }
    l2_flush_run();  // a burst breaks the single-word run (both the direct and chunked paths)
    ++g_fifoBurstCalls;
    g_fifoBurstBytes += sizeBytes;

    if (IsDisplayListActive() && WriteDisplayListBurst(data, sizeBytes)) {
        return;
    }

    const uint32_t applied = ApplyFifoPacketsDirect(data, sizeBytes);
    if (applied < sizeBytes) {
        HleFifoWriteBurstChunked(data + applied, sizeBytes - applied);
    }
}

// Every CP register write the runtime decodes (raw stream, burst writes, display-list replays) is also handed to
// Aurora, which keeps its own vertex layout (VCD/VAT/arrays) for display-list playback. Without it a layout set
// natively or by a replayed state list never reached Aurora, and lists were walked with the wrong vertex size (NO100F
// level geometry: INDEX16 lists decoded as DIRECT, garbage geometry and bogus XF writes).
void GxCpDecode::GxCpDecodeForwardToAurora(uint8_t reg, uint32_t value) {
    const uint8_t packet[6] = {0x08, reg, uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
    GXCallDisplayList(packet, sizeof(packet));
}
