// Stage 0 probe (build round 1, per STAGE0-DESK.md): native overrides for
// GXCallDisplayList (0x80102464) and iModelCull (0x8000E220) that log the
// display-list call stream and the cull stream's render-callback targets.
//
// A wrap-and-call-original probe is impossible in this machinery:
// PPC_NATIVE_OVERRIDE makes the translator skip the address, so no
// translated original exists to call. These bodies are therefore faithful
// transliterations of the translator's own generated code for the two
// functions (extracted from the build shards and mechanically converted:
// static translated calls become direct calls, and the frustum-test call
// in iModelCull goes to the existing native override for 0x80168D74).
// Behaviour is the generated code's behaviour, line for line; the only
// additions are the two logging calls at entry. Logging is gated by the
// dk_diag key "s0probe" (NO100F_S0PROBE) and caps at 300 calls each.

#include "hle_stubs.h"
#include "no100f_native_scene.h"
#include "memory.h"
#include "ppc_runtime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

// The existing native frustum test that dispatch resolves 0x80168D74 to.
extern "C" void No100f_CameraFrustumTestSphere(CpuContext* ctx);

// Generated callees, declared directly (runtime TUs get no generated header).
extern "C" void func_800FE204(CpuContext* ctx);
extern "C" void func_800FE394(CpuContext* ctx);
extern "C" void func_80165BFC(CpuContext* ctx);

namespace {

bool S0ProbeEnabled() {
    static const bool enabled = std::getenv("NO100F_S0PROBE") != nullptr;
    return enabled;
}

// Caller-chain walk (shared by the Stage 0 log and the N2 DL census): at
// entry to GXCallDisplayList, gpr[1] is the caller's frame; each frame's +0
// is the back-chain and the next frame's +4 is its saved LR (the return
// address into that frame's caller). Returns the number of entries filled.
int S0ProbeCallerChain(CpuContext* ctx, uint32_t* chain, int maxChain) {
    int count = 0;
    uint32_t frame = ctx->gpr[1];
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

void S0ProbeLogDisplayList(CpuContext* ctx) {
    if (!S0ProbeEnabled()) return;
    static std::atomic<uint32_t> count{0};
    if (count.fetch_add(1) >= 300) return;
    uint32_t chain[6] = {};
    S0ProbeCallerChain(ctx, chain, 6);
    std::fprintf(stderr, "[s0probe] dl ptr=%08X size=%u lr=%08X chain=%08X,%08X,%08X,%08X,%08X,%08X\n",
                 ctx->gpr[3], ctx->gpr[4], ctx->lr,
                 chain[0], chain[1], chain[2], chain[3], chain[4], chain[5]);
    std::fflush(stderr);
}

void S0ProbeLogIModelCull(CpuContext* ctx) {
    if (!S0ProbeEnabled()) return;
    static std::atomic<uint32_t> count{0};
    if (count.fetch_add(1) >= 300) return;
    const uint32_t atomic = ctx->gpr[3];
    if (!atomic || !Memory::Contains(atomic, 0x4C)) return;
    const uint32_t cb = Memory::Read32(atomic + 0x48);
    const uint32_t geom = Memory::Read32(atomic + 0x18);
    uint32_t ntris = 0, nverts = 0;
    if (geom && Memory::Contains(geom, 0x60)) {
        ntris = Memory::Read32(geom + 0x10);
        nverts = Memory::Read32(geom + 0x14);
    }
    std::fprintf(stderr, "[s0probe] cull atomic=%08X cb=%08X geom=%08X ntris=%u nverts=%u\n",
                 atomic, cb, geom, ntris, nverts);
    if (geom && ntris == 76 && nverts == 152 && Memory::Contains(geom, 0x60)) {
        std::fprintf(stderr, "[s0probe] title geom dump:");
        for (uint32_t off = 0; off < 0x60; off += 4) {
            std::fprintf(stderr, " %08X", Memory::Read32(geom + off));
        }
        std::fprintf(stderr, "\n");
        const uint32_t materials = Memory::Read32(geom + 0x20);
        if (materials && Memory::Contains(materials, 4)) {
            const uint32_t material = Memory::Read32(materials);
            if (material && Memory::Contains(material, 4)) {
                const uint32_t texture = Memory::Read32(material);
                if (texture && Memory::Contains(texture, 0x54)) {
                    char name[32] = {};
                    for (int i = 0; i < 31; ++i) {
                        const uint8_t byte = Memory::Read8(texture + 0x10 + static_cast<uint32_t>(i));
                        if (byte == 0) break;
                        name[i] = static_cast<char>(byte);
                    }
                    std::fprintf(stderr, "[s0probe] title tex=%08X name=\"%s\" w18=%08X w50=%08X\n",
                                 texture, name, Memory::Read32(texture + 0x18),
                                 Memory::Read32(texture + 0x50));
                }
            }
        }
    }
    std::fflush(stderr);
}

}  // namespace

extern "C" void No100f_ProbeGXCallDisplayList(CpuContext* ctx)
{
    S0ProbeLogDisplayList(ctx);
    // N2 DL census: note this submission's caller (guest return address +
    // saved-LR chain) for the player's caller-keyed counters. Gated on the
    // census key inside; inert when off.
    if (No100fDlCensusEnabled()) {
        uint32_t chain[6] = {};
        const int chainCount = S0ProbeCallerChain(ctx, chain, 6);
        No100fDlCensusNoteCall(ctx->gpr[3], ctx->gpr[4], ctx->lr, chain, chainCount);
    }
    No100fNativeSceneNoteGuestDisplayList(ctx->gpr[3], ctx->gpr[4]);
    uint32_t cr0_0 = 0;

    uint32_t r0 = ctx->gpr[0];
    uint32_t r1 = ctx->gpr[1];
    uint32_t r3 = ctx->gpr[3];
    uint32_t r4 = ctx->gpr[4];
    uint32_t r5 = ctx->gpr[5];
    uint32_t r13 = ctx->gpr[13];
    uint32_t r30 = ctx->gpr[30];
    uint32_t r31 = ctx->gpr[31];
    uint32_t cr = ctx->cr;
    uint32_t xer = ctx->xer;

    goto loc_80102464;

loc_80102464:
{
    r0 = ctx->lr;
    MemoryInline::FlatWriteRam32((r1 + 4), r0);
    MemoryInline::FlatWriteRam32((r1 + -24), r1);
    r1 = (r1 + -24);
    MemoryInline::FlatWriteRam32((r1 + 20), r31);
    r31 = (r4 + 0);
    MemoryInline::FlatWriteRam32((r1 + 16), r30);
    r30 = (r3 + 0);
    r5 = MemoryInline::FlatRead32((r13 + -32184));
    r0 = MemoryInline::FlatRead32((r5 + 1268));
    SetCRResident(cr, xer, 0, static_cast<uint32_t>(r0), static_cast<uint32_t>(0));
}

loc_8010248C:
{
    if (((cr & 0x20000000u) != 0)) {
        goto loc_80102494;
    }
}

loc_80102490:
{
    ctx->lr = 0x80102494u;
    ctx->gpr[0] = r0;
    ctx->gpr[1] = r1;
    ctx->gpr[3] = r3;
    ctx->gpr[5] = r5;
    ctx->gpr[30] = r30;
    ctx->gpr[31] = r31;
    ctx->cr = cr;
    func_800FE204(ctx);
    r0 = ctx->gpr[0];
    r1 = ctx->gpr[1];
    r3 = ctx->gpr[3];
    r4 = ctx->gpr[4];
    r5 = ctx->gpr[5];
    r30 = ctx->gpr[30];
    r31 = ctx->gpr[31];
    cr = ctx->cr;
    xer = ctx->xer;
}

loc_80102494:
{
    r3 = MemoryInline::FlatRead32((r13 + -32184));
    r0 = MemoryInline::FlatRead32(r3);
    SetCRResident(cr, xer, 0, static_cast<uint32_t>(r0), static_cast<uint32_t>(0));
}

loc_801024A0:
{
    if (((cr & 0x20000000u) == 0)) {
        goto loc_801024A8;
    }
}

loc_801024A4:
{
    ctx->gpr[0] = r0;
    ctx->gpr[1] = r1;
    ctx->gpr[3] = r3;
    ctx->gpr[5] = r5;
    ctx->cr = cr;
    func_800FE394(ctx);
    r0 = ctx->gpr[0];
    r3 = ctx->gpr[3];
    r4 = ctx->gpr[4];
    r5 = ctx->gpr[5];
    cr = ctx->cr;
}

loc_801024A8:
{
    r0 = 64;
    r3 = -872349696;
    GX_HLE_FIFO_Write8(static_cast<uint8_t>(r0));
    GX_HLE_FIFO_Write32(static_cast<uint32_t>(r30));
    GX_HLE_FIFO_Write32(static_cast<uint32_t>(r31));
    r0 = MemoryInline::FlatRead32((r1 + 28));
    r31 = MemoryInline::FlatRead32((r1 + 20));
    r30 = MemoryInline::FlatRead32((r1 + 16));
    r1 = (r1 + 24);
    ctx->lr = r0;
    ctx->gpr[0] = r0;
    ctx->gpr[1] = r1;
    ctx->gpr[3] = r3;
    ctx->gpr[5] = r5;
    ctx->gpr[30] = r30;
    ctx->gpr[31] = r31;
    ctx->cr = cr;
    return;
}

}

extern "C" void No100f_ProbeIModelCull(CpuContext* ctx)
{
    S0ProbeLogIModelCull(ctx);
    No100fNativeScenePinScanCull(ctx->gpr[3]);
    uint32_t cr0_0 = 0;
    uint32_t r3_rot_0 = 0;
    uint8_t* guest_range_0 = nullptr;

    uint32_t r0 = ctx->gpr[0];
    uint32_t r1 = ctx->gpr[1];
    uint32_t r2 = ctx->gpr[2];
    uint32_t r3 = ctx->gpr[3];
    uint32_t r4 = ctx->gpr[4];
    uint32_t r5 = ctx->gpr[5];
    uint32_t r6 = ctx->gpr[6];
    uint32_t r13 = ctx->gpr[13];
    uint32_t r29 = ctx->gpr[29];
    uint32_t r30 = ctx->gpr[30];
    uint32_t r31 = ctx->gpr[31];
    PPC_FPR f0 = ctx->fpr[0];
    PPC_FPR f1 = ctx->fpr[1];
    PPC_FPR f2 = ctx->fpr[2];
    PPC_FPR f3 = ctx->fpr[3];
    PPC_FPR f4 = ctx->fpr[4];
    PPC_FPR f5 = ctx->fpr[5];
    PPC_FPR f6 = ctx->fpr[6];
    PPC_FPR f7 = ctx->fpr[7];
    PPC_FPR f8 = ctx->fpr[8];
    uint32_t cr = ctx->cr;

    goto loc_8000E220;

loc_8000E220:
{
    MemoryInline::FlatWriteRam32((r1 + -48), r1);
    r1 = (r1 + -48);
    r0 = ctx->lr;
    MemoryInline::FlatWriteRam32((r1 + 52), r0);
    MemoryInline::FlatWriteRam32((r1 + 36), r29);
    MemoryInline::FlatWriteRam32((r1 + 40), r30);
    MemoryInline::FlatWriteRam32((r1 + 44), r31);
    r29 = r3;
    r30 = r4;
    r3 = (r1 + 12);
    r6 = r30;
    r4 = (r29 + 28);
    r5 = MemoryInline::FlatRead32((r13 + -27652));
    r31 = MemoryInline::FlatRead32(r5);
    r5 = 1;
    ctx->lr = 0x8000E254u;
    ctx->gpr[0] = r0;
    ctx->gpr[1] = r1;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[6] = r6;
    ctx->gpr[29] = r29;
    ctx->gpr[30] = r30;
    ctx->gpr[31] = r31;
    ctx->fpr[0] = f0;
    ctx->fpr[1] = f1;
    ctx->fpr[2] = f2;
    ctx->fpr[3] = f3;
    ctx->fpr[4] = f4;
    ctx->fpr[5] = f5;
    ctx->fpr[6] = f6;
    ctx->fpr[7] = f7;
    ctx->fpr[8] = f8;
    ctx->cr = cr;
    func_80165BFC(ctx);
    r0 = ctx->gpr[0];
    r1 = ctx->gpr[1];
    r2 = ctx->gpr[2];
    r3 = ctx->gpr[3];
    r4 = ctx->gpr[4];
    r5 = ctx->gpr[5];
    r6 = ctx->gpr[6];
    r13 = ctx->gpr[13];
    r29 = ctx->gpr[29];
    r30 = ctx->gpr[30];
    r31 = ctx->gpr[31];
    f0 = ctx->fpr[0];
    f1 = ctx->fpr[1];
    f2 = ctx->fpr[2];
    f3 = ctx->fpr[3];
    f4 = ctx->fpr[4];
    f5 = ctx->fpr[5];
    f6 = ctx->fpr[6];
    f7 = ctx->fpr[7];
    f8 = ctx->fpr[8];
    cr = ctx->cr;
    guest_range_0 = MemoryInline::ResolveRangeHost(r30, 0, 44u, true, false);
    {
        const auto resolved_pair = MemoryInline::ReadResolvedPair32(guest_range_0, 16u);
        if (resolved_pair.valid) {
            f1.d = PpcBitCastToFloatInline(resolved_pair.first);
            f0.d = PpcBitCastToFloatInline(resolved_pair.second);
        } else {
            f1.d = MemoryInline::ReadResolvedFloat32(guest_range_0, 16u, (r30 + 16));
            f0.d = MemoryInline::ReadResolvedFloat32(guest_range_0, 20u, (r30 + 20));
        }
    }
    f6.d = MemoryInline::ReadResolvedFloat32(guest_range_0, 32u, (r30 + 32));
    f3.d = PpcFmulsInline(f1.d, f1.d);
    f5.d = MemoryInline::ReadResolvedFloat32(guest_range_0, 36u, (r30 + 36));
    f2.d = PpcFmulsInline(f0.d, f0.d);
    f4.d = MemoryInline::ReadResolvedFloat32(guest_range_0, 24u, (r30 + 24));
    f1.d = PpcFmulsInline(f6.d, f6.d);
    f0.d = PpcFmulsInline(f5.d, f5.d);
    f8.d = MemoryInline::ReadResolvedFloat32(guest_range_0, 40u, (r30 + 40));
    f4.d = PpcFmulsInline(f4.d, f4.d);
    f5.d = MemoryInline::ReadResolvedFloat32(guest_range_0, 0u, r30);
    f3.d = static_cast<double>(PpcForceSingleValueInline(f3.d + f2.d));
    {
        const auto resolved_pair = MemoryInline::ReadResolvedPair32(guest_range_0, 4u);
        if (resolved_pair.valid) {
            f6.d = PpcBitCastToFloatInline(resolved_pair.first);
            f7.d = PpcBitCastToFloatInline(resolved_pair.second);
        } else {
            f6.d = MemoryInline::ReadResolvedFloat32(guest_range_0, 4u, (r30 + 4));
            f7.d = MemoryInline::ReadResolvedFloat32(guest_range_0, 8u, (r30 + 8));
        }
    }
    f2.d = PpcFmulsInline(f8.d, f8.d);
    f0.d = static_cast<double>(PpcForceSingleValueInline(f1.d + f0.d));
    f5.d = PpcFmulsInline(f5.d, f5.d);
    f1.d = PpcFmulsInline(f6.d, f6.d);
    f3.d = static_cast<double>(PpcForceSingleValueInline(f4.d + f3.d));
    f4.d = static_cast<double>(PpcForceSingleValueInline(f2.d + f0.d));
    f2.d = PpcFmulsInline(f7.d, f7.d);
    f0.d = static_cast<double>(PpcForceSingleValueInline(f5.d + f1.d));
    SetCRFloatResident(cr, 0, f3.d, f4.d);
}

loc_8000E2B4:
{
    f1.d = static_cast<double>(PpcForceSingleValueInline(f2.d + f0.d));
    cr = PpcCrLogicalResident(cr, static_cast<uint32_t>(7), static_cast<uint32_t>(2), static_cast<uint32_t>(1), static_cast<uint32_t>(2));
    if (((cr & 0x20000000u) == 0)) {
        goto loc_8000E2C8;
    }
}

loc_8000E2C0:
{
    f0.d = f3.d;
    goto loc_8000E2CC;
}

loc_8000E2C8:
{
    f0.d = f4.d;
}

loc_8000E2CC:
{
    SetCRFloatResident(cr, 0, f1.d, f0.d);
}

loc_8000E2D0:
{
    cr = PpcCrLogicalResident(cr, static_cast<uint32_t>(7), static_cast<uint32_t>(2), static_cast<uint32_t>(1), static_cast<uint32_t>(2));
    if (((cr & 0x20000000u) == 0)) {
        goto loc_8000E2E0;
    }
}

loc_8000E2D8:
{
    f4.d = f1.d;
    goto loc_8000E2F0;
}

loc_8000E2E0:
{
    SetCRFloatResident(cr, 0, f3.d, f4.d);
}

loc_8000E2E4:
{
    cr = PpcCrLogicalResident(cr, static_cast<uint32_t>(7), static_cast<uint32_t>(2), static_cast<uint32_t>(1), static_cast<uint32_t>(2));
    if (((cr & 0x20000000u) == 0)) {
        goto loc_8000E2F0;
    }
}

loc_8000E2EC:
{
    f4.d = f3.d;
}

loc_8000E2F0:
{
    f0.d = MemoryInline::FlatReadFloat32((r2 + -32432));
    SetCRFloatResident(cr, 0, f4.d, f0.d);
}

loc_8000E2F8:
{
    if (((cr & 0x40000000u) == 0)) {
        goto loc_8000E348;
    }
}

loc_8000E2FC:
{
    f1.d = PPC_Frsqrte(f4.d);
    f3.d = MemoryInline::FlatReadFloat64((r2 + -32424));
    f2.d = MemoryInline::FlatReadFloat64((r2 + -32416));
    f0.d = (f1.d * f1.d);
    f1.d = (f3.d * f1.d);
    f0.d = PpcFnmsubInline(f4.d, f0.d, f2.d);
    f1.d = (f1.d * f0.d);
    f0.d = (f1.d * f1.d);
    f1.d = (f3.d * f1.d);
    f0.d = PpcFnmsubInline(f4.d, f0.d, f2.d);
    f1.d = (f1.d * f0.d);
    f0.d = (f1.d * f1.d);
    f1.d = (f3.d * f1.d);
    f0.d = PpcFnmsubInline(f4.d, f0.d, f2.d);
    f0.d = (f1.d * f0.d);
    f0.d = (f4.d * f0.d);
    f0.d = static_cast<double>(PpcForceSingleValueInline(f0.d));
    MemoryInline::FlatWriteRamFloat32((r1 + 8), f0.d);
    f4.d = MemoryInline::FlatReadFloat32((r1 + 8));
}

loc_8000E348:
{
    f0.d = MemoryInline::FlatReadFloat32((r29 + 40));
    r3 = r31;
    r5 = MemoryInline::FlatRead32((r1 + 12));
    r4 = (r1 + 12);
    f0.d = PpcFmulsInline(f0.d, f4.d);
    r0 = MemoryInline::FlatRead32((r1 + 16));
    MemoryInline::FlatWriteRamFloat32((r1 + 24), f0.d);
    MemoryInline::FlatWrite32((r29 + 44), r5);
    MemoryInline::FlatWrite32((r29 + 48), r0);
    r0 = MemoryInline::FlatRead32((r1 + 20));
    MemoryInline::FlatWrite32((r29 + 52), r0);
    f0.d = MemoryInline::FlatReadFloat32((r1 + 24));
    MemoryInline::FlatWriteFloat32((r29 + 56), f0.d);
    ctx->lr = 0x8000E380u;
    ctx->gpr[0] = r0;
    ctx->gpr[1] = r1;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[6] = r6;
    ctx->gpr[29] = r29;
    ctx->gpr[30] = r30;
    ctx->gpr[31] = r31;
    ctx->fpr[0] = f0;
    ctx->fpr[1] = f1;
    ctx->fpr[2] = f2;
    ctx->fpr[3] = f3;
    ctx->fpr[4] = f4;
    ctx->fpr[5] = f5;
    ctx->fpr[6] = f6;
    ctx->fpr[7] = f7;
    ctx->fpr[8] = f8;
    ctx->cr = cr;
    No100f_CameraFrustumTestSphere(ctx);
    r0 = ctx->gpr[0];
    r1 = ctx->gpr[1];
    r2 = ctx->gpr[2];
    r3 = ctx->gpr[3];
    r4 = ctx->gpr[4];
    r5 = ctx->gpr[5];
    r6 = ctx->gpr[6];
    r13 = ctx->gpr[13];
    r29 = ctx->gpr[29];
    r30 = ctx->gpr[30];
    r31 = ctx->gpr[31];
    f0 = ctx->fpr[0];
    f1 = ctx->fpr[1];
    f2 = ctx->fpr[2];
    f3 = ctx->fpr[3];
    f4 = ctx->fpr[4];
    f5 = ctx->fpr[5];
    f6 = ctx->fpr[6];
    f7 = ctx->fpr[7];
    f8 = ctx->fpr[8];
    cr = ctx->cr;
    r0 = PPC_CntlzwInline(static_cast<uint32_t>(r3));
    r29 = MemoryInline::FlatRead32((r1 + 36));
    r30 = MemoryInline::FlatRead32((r1 + 40));
    r31 = MemoryInline::FlatRead32((r1 + 44));
    r3_rot_0 = PpcRotl32Inline(static_cast<uint32_t>(r0), static_cast<uint32_t>(27));
    r3 = (r3_rot_0 & 255);
    r0 = MemoryInline::FlatRead32((r1 + 52));
    ctx->lr = r0;
    r1 = (r1 + 48);
    ctx->gpr[0] = r0;
    ctx->gpr[1] = r1;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[6] = r6;
    ctx->gpr[29] = r29;
    ctx->gpr[30] = r30;
    ctx->gpr[31] = r31;
    ctx->fpr[0] = f0;
    ctx->fpr[1] = f1;
    ctx->fpr[2] = f2;
    ctx->fpr[3] = f3;
    ctx->fpr[4] = f4;
    ctx->fpr[5] = f5;
    ctx->fpr[6] = f6;
    ctx->fpr[7] = f7;
    ctx->fpr[8] = f8;
    ctx->cr = cr;
    return;
}

}
PPC_NATIVE_OVERRIDE_VOID(80102464, No100f_ProbeGXCallDisplayList, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(8000E220, No100f_ProbeIModelCull, (CpuContext* ctx), (ctx));
