// NO100F: native NEON replacement for the RpSkin kernel func_80144E8C.
//
// The kernel is a 4-bone skinning kernel (RenderWare RpSkin), called by the
// wrapper func_80145358 once per skinned geometry. It is the L3 lever of the
// N8 diagnosis (pool 2.18 ms/frame, prize 0.5-1.6 ms). The float order and
// the control flow are implemented in include/hle/no100f_rpskin.h, which the
// differential test (tests/no100f_rpskin_tests.cpp) proves bit-exact against
// a scalar reference written from the generated kernel body.
//
// This file is the CpuContext* wrapper: it extracts the kernel's parameters
// from the guest register file (r3-r10, the three stack arguments at
// [sp+8]/[sp+12]/[sp+16], GQR0/6/7), decodes the GQRs, resolves the guest
// streams to host pointers, and calls the core. Anything it cannot handle
// falls back to the translated kernel for that call, so the native can never
// produce a wrong answer for a format or layout it does not know:
//
//   - GQR0 (matrices) is not float type 0 scale 0.
//   - Either stream's load or store GQR type is not F32 (0) or S16 (5).
//   - Any stream's host-pointer resolve fails (unmapped, checked-access, or
//     a span over the 1 MiB resolve limit).
//   - A stack argument cannot be read.
//
// The wrapper also reproduces the kernel's post-call register state, so the
// caller (func_80145358) sees exactly what the translated kernel leaves:
// the advanced stream pointers in r3-r7/r9, the last index word in r11,
// CR0/CR5/CR6/CR7, and ctr. It does not touch the FPRs (the kernel restores
// the nonvolatile ones and the volatile ones are call-clobbered scratch) or
// the callee-saved GPRs (never modified).
//
// The plugin-offset global (0x80271618) and the skin struct ([0x801EDDE8])
// are NOT read here: the wrapper already resolved them into the parameters
// the kernel receives (weights, indices, palette, streams, count, strides).
// The palette is the guest's own composed palette; the native never
// composes, reorders, or caches it.

#include <cstdint>

#include "abi_bridge.h"
#include "hle/no100f_rpskin.h"
#include "memory.h"
#include "memory_access.h"

extern "C" void func_80144E8C(CpuContext* ctx);  // the game's skin kernel, kept translated

namespace {

using namespace No100fRpSkin;

// One CR field (0-7) from a comparison, preserving the SO bit. A field sits
// at bits (28 - 4*field); within it LT is the top bit, then GT, EQ, SO.
uint32_t SetCrField(uint32_t cr, int field, bool lt, bool gt, bool eq) {
    const uint32_t shift = 28u - 4u * static_cast<uint32_t>(field);
    const uint32_t so = cr & (0x1u << shift);
    uint32_t bits = so;
    if (lt) bits |= (0x8u << shift);
    if (gt) bits |= (0x4u << shift);
    if (eq) bits |= (0x2u << shift);
    return (cr & ~(0xFu << shift)) | bits;
}

uint32_t SetCrFieldInt(uint32_t cr, int field, int32_t a, int32_t b) {
    return SetCrField(cr, field, a < b, a > b, a == b);
}

uint32_t SetCrFieldFloat(uint32_t cr, int field, float a, float b) {
    // IEEE comparison: a NaN operand makes LT, GT and EQ all false.
    return SetCrField(cr, field, a < b, a > b, a == b);
}

// The byte span a stream covers over count vertices, or 0 when it cannot be
// bounded (overflow, or over the 1 MiB resolve limit). vertexStride is the
// per-vertex advance; the last vertex's components sit compStride apart.
uint64_t StreamSpan(uint32_t count, uint32_t vertexStride, uint32_t compStride, uint32_t elemSize) {
    if (count == 0u) return 0;
    const uint64_t vertices = static_cast<uint64_t>(count) - 1u;
    const uint64_t span = vertices * static_cast<uint64_t>(vertexStride) +
                          2u * static_cast<uint64_t>(compStride) + static_cast<uint64_t>(elemSize);
    if (span == 0u || span > 0x100000u) return 0;
    return span;
}

}  // namespace

extern "C" void No100f_RpSkinKernel(CpuContext* ctx) {
    const uint32_t weightsPtr = ctx->gpr[3];
    const uint32_t inPosPtr = ctx->gpr[4];
    const uint32_t inNrmPtr = ctx->gpr[5];
    const uint32_t outPosPtr = ctx->gpr[6];
    const uint32_t outNrmPtr = ctx->gpr[7];
    const uint32_t palettePtr = ctx->gpr[8];
    const uint32_t indicesPtr = ctx->gpr[9];
    const int32_t count = static_cast<int32_t>(ctx->gpr[10]);
    const uint32_t sp = ctx->gpr[1];

    // The kernel's three stack arguments sit at [sp+8]/[sp+12]/[sp+16]: its
    // prologue subtracts 184 from sp and reads [sp+192]/[sp+196]/[sp+200].
    uint32_t posStride = 0;
    uint32_t nrmStride = 0;
    uint32_t nrmExtra = 0;
    if (!Memory::TryRead32(sp + 8u, posStride) || !Memory::TryRead32(sp + 12u, nrmStride) ||
        !Memory::TryRead32(sp + 16u, nrmExtra)) {
        func_80144E8C(ctx);
        return;
    }

    // Post-call CR0 for the early exit is compare(count, 0); the kernel sets
    // it in its prologue before the count check.
    if (count <= 0) {
        ctx->gpr[3] = weightsPtr - 4u;
        ctx->gpr[4] = inPosPtr - posStride;
        ctx->gpr[5] = inNrmPtr - nrmStride;
        ctx->gpr[6] = outPosPtr - posStride;
        ctx->gpr[7] = outNrmPtr - nrmStride;
        ctx->gpr[9] = indicesPtr - 4u;
        ctx->cr = SetCrFieldInt(ctx->cr, 0, count, 0);
        ctx->ctr = static_cast<uint32_t>(count);
        return;
    }
    const uint32_t ucount = static_cast<uint32_t>(count);

    // GQR0 (matrices) must be float type 0 scale 0; the kernel never stores
    // matrices, so only its load halfword matters.
    const uint32_t gqr0 = ctx->gqr[0];
    if (((gqr0 >> 16) & 0x7u) != 0u || ((gqr0 >> 24) & 0x3Fu) != 0u) {
        func_80144E8C(ctx);
        return;
    }

    // Decode each stream's load and store sides independently.
    SkinParams params;
    if (!DecodeGqr(ctx->gqr[6], true, params.posLoadKind, params.posLoadScale) ||
        !DecodeGqr(ctx->gqr[6], false, params.posStoreKind, params.posStoreScale) ||
        !DecodeGqr(ctx->gqr[7], true, params.nrmLoadKind, params.nrmLoadScale) ||
        !DecodeGqr(ctx->gqr[7], false, params.nrmStoreKind, params.nrmStoreScale)) {
        func_80144E8C(ctx);
        return;
    }

    // hasNormals is the kernel's r4-vs-r5 pointer test, taken once before the
    // loop on the pre-decremented pointers.
    const uint32_t inPosPre = inPosPtr - posStride;
    const uint32_t inNrmPre = inNrmPtr - nrmStride;
    params.hasNormals = inPosPre != inNrmPre;

    params.count = count;
    params.posStride = posStride;
    params.nrmStride = nrmStride;
    params.nrmExtra = nrmExtra;

    // Resolve every stream the core touches to a host pointer. Any failure
    // falls back to the translated kernel for the whole call.
    const uint64_t weightsLen = static_cast<uint64_t>(ucount) * 16u;
    if (weightsLen == 0u || weightsLen > 0x100000u) {
        func_80144E8C(ctx);
        return;
    }
    uint8_t* weightsHost =
        MemoryInline::ResolveRangeHost(weightsPtr, 0, static_cast<uint32_t>(weightsLen), true, false);
    if (weightsHost == nullptr) {
        func_80144E8C(ctx);
        return;
    }
    const uint64_t indicesLen = static_cast<uint64_t>(ucount) * 4u;
    if (indicesLen == 0u || indicesLen > 0x100000u) {
        func_80144E8C(ctx);
        return;
    }
    uint8_t* indicesHost =
        MemoryInline::ResolveRangeHost(indicesPtr, 0, static_cast<uint32_t>(indicesLen), true, false);
    if (indicesHost == nullptr) {
        func_80144E8C(ctx);
        return;
    }
    // The palette is 256 matrices of 64 B. The bones actually referenced are
    // a subset, but the full table is small and the resolve is one call.
    uint8_t* paletteHost = MemoryInline::ResolveRangeHost(palettePtr, 0, 16384u, true, false);
    if (paletteHost == nullptr) {
        func_80144E8C(ctx);
        return;
    }
    const uint32_t posElem = ElemSize(params.posLoadKind);
    const uint32_t posStoreElem = ElemSize(params.posStoreKind);
    const uint64_t posSpan = StreamSpan(ucount, 3u * posStride, posStride, posElem);
    const uint64_t posOutSpan = StreamSpan(ucount, 3u * posStride, posStride, posStoreElem);
    if (posSpan == 0u || posOutSpan == 0u) {
        func_80144E8C(ctx);
        return;
    }
    uint8_t* inPosHost =
        MemoryInline::ResolveRangeHost(inPosPtr, 0, static_cast<uint32_t>(posSpan), true, false);
    uint8_t* outPosHost =
        MemoryInline::ResolveRangeHost(outPosPtr, 0, static_cast<uint32_t>(posOutSpan), true, true);
    if (inPosHost == nullptr || outPosHost == nullptr) {
        func_80144E8C(ctx);
        return;
    }
    uint8_t* inNrmHost = nullptr;
    uint8_t* outNrmHost = nullptr;
    if (params.hasNormals) {
        const uint32_t nrmElem = ElemSize(params.nrmLoadKind);
        const uint32_t nrmStoreElem = ElemSize(params.nrmStoreKind);
        const uint32_t nrmVertexStride = 3u * nrmStride + nrmExtra;
        const uint64_t nrmSpan = StreamSpan(ucount, nrmVertexStride, nrmStride, nrmElem);
        const uint64_t nrmOutSpan = StreamSpan(ucount, nrmVertexStride, nrmStride, nrmStoreElem);
        if (nrmSpan == 0u || nrmOutSpan == 0u) {
            func_80144E8C(ctx);
            return;
        }
        inNrmHost =
            MemoryInline::ResolveRangeHost(inNrmPtr, 0, static_cast<uint32_t>(nrmSpan), true, false);
        outNrmHost = MemoryInline::ResolveRangeHost(outNrmPtr, 0, static_cast<uint32_t>(nrmOutSpan),
                                                    true, true);
        if (inNrmHost == nullptr || outNrmHost == nullptr) {
            func_80144E8C(ctx);
            return;
        }
    }

    params.weights = weightsHost;
    params.indices = indicesHost;
    params.palette = paletteHost;
    params.inPos = inPosHost;
    params.inNrm = inNrmHost;
    params.outPos = outPosHost;
    params.outNrm = outNrmHost;

    SkinCore(params);

    // Post-call register state, exactly as the translated kernel leaves it.
    // Each stream pointer advances once per vertex past its last element,
    // from its pre-decremented start.
    const uint32_t nrmVertexStride = 3u * nrmStride + nrmExtra;
    ctx->gpr[3] = weightsPtr - 4u + 16u * ucount;
    ctx->gpr[4] = inPosPre + 3u * posStride * ucount;
    ctx->gpr[5] = inNrmPre + nrmVertexStride * ucount;
    ctx->gpr[6] = (outPosPtr - posStride) + 3u * posStride * ucount;
    ctx->gpr[7] = (outNrmPtr - nrmStride) + nrmVertexStride * ucount;
    ctx->gpr[9] = indicesPtr - 4u + 4u * ucount;
    ctx->gpr[11] = LoadU32BE(indicesHost + static_cast<size_t>(ucount - 1u) * 4u);
    uint32_t cr = ctx->cr;
    cr = SetCrFieldInt(cr, 0, static_cast<int32_t>(inPosPre), static_cast<int32_t>(inNrmPre));
    const float w1 = LoadF32BE(weightsHost + static_cast<size_t>(ucount - 1u) * 16u + 4u);
    const float w2 = LoadF32BE(weightsHost + static_cast<size_t>(ucount - 1u) * 16u + 8u);
    const float w3 = LoadF32BE(weightsHost + static_cast<size_t>(ucount - 1u) * 16u + 12u);
    cr = SetCrFieldFloat(cr, 5, w1, 0.0f);
    cr = SetCrFieldFloat(cr, 6, w2, 0.0f);
    cr = SetCrFieldFloat(cr, 7, w3, 0.0f);
    ctx->cr = cr;
    ctx->ctr = 0;
}

REGISTER_NATIVE_FUNCTION_AS(0x80144E8C, No100f_RpSkinKernel, "No100f_RpSkinKernel");
