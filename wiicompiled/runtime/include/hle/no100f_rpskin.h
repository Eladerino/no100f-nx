#pragma once
// NO100F: native replacement for the RpSkin kernel func_80144E8C.
//
// The kernel is a 4-bone skinning kernel (RenderWare RpSkin). Per vertex it
// reads 4 F32 weights and 4 bone indices packed in one word, transforms the
// position (3x4 matrix product with translation) and the normal (3x3 rotation
// only) by each bone's matrix from the guest-composed palette, weights each
// bone's result and sums them. The guest body was read in full from the
// generated shard (build_shards/base_common/shard_d08f2756ebb3fe397e55be6c.cpp,
// func_80144E8C at :29326); this file implements exactly that body.
//
// CORRECTION to the N8 design doc (N8-L2L3L4-DESIGN-PASSES.md L3 section):
// the doc says a zero weight collapses bone j to bone 0's matrix and the bone
// is still evaluated ("do NOT skip the bone"). The generated body does the
// opposite. The collapse assignments (r15/r16/r17 = r14 at loc_80144F7C/80/84)
// are DEAD: each bone block ends in a chained skip to the stores
// (loc_80144F88 tests CR5.GT, i.e. w1 > 0; loc_80145028 tests CR6.GT, i.e.
// w2 > 0; loc_80145090 tests CR7.GT, i.e. w3 > 0). Bone j is evaluated if and
// only if ALL of w1..wj are > 0 (bone 0 always). If w1 <= 0, bones 1-3 are
// all skipped even when w2 or w3 is positive; if w1 > 0 but w2 <= 0, bones 2-3
// are skipped even when w3 is positive. The collapsed pointers are never
// used, because the skip jumps past the bone that would have used them. This
// file implements the chained skip, which is what the guest does. The
// differential test (tests/no100f_rpskin_tests.cpp) covers the non-sorted
// weight cases (w1 = 0 with w2 > 0, negative weights, NaN weights) that
// distinguish the skip from the collapse.
//
// Float order, per bone j with matrix M (16 F32, row-major 4x4, 64 B stride;
// only the 3x4 submatrix rows 0-2 is used, the 4th column is never loaded):
//   px = fma(M[8], z, fma(M[4], y, fma(M[0], x, M[12])))
//   py = fma(M[9], z, fma(M[5], y, fma(M[1], x, M[13])))
//   pz = fma(M[10], z, fma(M[6], y, fma(M[2], x, M[14])))
//   nx = fma(M[8], nz, fma(M[4], ny, M[0] * nx))
//   ny = fma(M[9], nz, fma(M[5], ny, M[1] * nx))
//   nz = fma(M[10], nz, fma(M[6], ny, M[2] * nx))
// The position seed is the translation (an FMA with the translation as the
// addend); the normal seed is a plain multiply (no translation). Bone 0 is
// weighted by a plain multiply (acc = bone0 * w0); bones 1-3 are weighted by
// a fused multiply-add (acc = fma(bone_j, wj, acc)). The NEON path below
// computes the three components in parallel lanes of one float32x4_t, which
// is lane-independent and therefore bit-identical to the guest's per-lane
// paired-single arithmetic. The 4th lane carries the unused 4th matrix
// column and is never stored.
//
// GQR handling: the position stream uses GQR6 and the normal stream GQR7.
// Each stream's load side (type/scale) and store side (type/scale) are
// decoded independently. F32 (type 0) and S16 (type 5, signed) are handled;
// any other type is NOT handled here and the caller must fall back to the
// guest kernel for that call. The S16 dequantize/quantize formulas are the
// runtime's PpcDequantizePsqInline / PpcScaleAndClampPsqInline, copied
// exactly (ldexp scale factors, clamp, truncation).
//
// Byte order: all pointers here address GUEST memory in guest (big-endian)
// byte order, as resolved by the caller from guest addresses. Every
// multi-byte load/store byte-swaps. The differential test provides its
// buffers in guest byte order too, so the core and the reference see the
// same bytes.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace No100fRpSkin {

// One stream's element encoding, from one GQR halfword.
enum class ElemKind : uint8_t {
    kFloat32 = 0,  // GQR type 0
    kS16 = 5,      // GQR type 5 (signed 16-bit)
};

// Decode one GQR halfword. loadSide selects the load halfword (bits 16-31)
// or the store halfword (bits 0-15). Returns false for a type the native
// does not handle (anything other than 0 or 5); the caller falls back.
inline bool DecodeGqr(uint32_t gqr, bool loadSide, ElemKind& kind, uint32_t& scale) {
    uint32_t type = 0;
    if (loadSide) {
        type = (gqr >> 16) & 0x7u;
        scale = (gqr >> 24) & 0x3Fu;
    } else {
        type = gqr & 0x7u;
        scale = (gqr >> 8) & 0x3Fu;
    }
    if (type == 0u) {
        kind = ElemKind::kFloat32;
        return true;
    }
    if (type == 5u) {
        kind = ElemKind::kS16;
        return true;
    }
    return false;
}

// S16 dequantize: the runtime's PpcDequantizePsqInline, copied exactly.
inline float DequantizeS16(int16_t value, uint32_t scale) {
    if (scale == 0u) {
        return static_cast<float>(value);
    }
    const float factor = scale < 32u
        ? std::ldexp(1.0f, -static_cast<int>(scale))
        : std::ldexp(1.0f, static_cast<int>(64u - scale));
    return static_cast<float>(value) * factor;
}

// S16 quantize: the runtime's PpcScaleAndClampPsqInline<int16_t>, copied
// exactly (scale-0 branch included: no multiply, clamp the value itself).
inline int16_t QuantizeS16(float value, uint32_t scale) {
    if (scale == 0u) {
        constexpr float kMin = static_cast<float>(INT16_MIN);
        constexpr float kMax = static_cast<float>(INT16_MAX);
        return static_cast<int16_t>(std::clamp(value, kMin, kMax));
    }
    const float factor = scale < 32u
        ? std::ldexp(1.0f, static_cast<int>(scale))
        : std::ldexp(1.0f, -(64 - static_cast<int>(scale)));
    const float scaled = value * factor;
    constexpr float kMin = static_cast<float>(INT16_MIN);
    constexpr float kMax = static_cast<float>(INT16_MAX);
    return static_cast<int16_t>(std::clamp(scaled, kMin, kMax));
}

// Guest-order (big-endian) loads and stores. memcpy avoids alignment
// assumptions; the byte swap is the guest-to-host conversion.
inline float LoadF32BE(const uint8_t* p) {
    uint32_t bits = 0;
    std::memcpy(&bits, p, sizeof(bits));
    bits = __builtin_bswap32(bits);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void StoreF32BE(uint8_t* p, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bits = __builtin_bswap32(bits);
    std::memcpy(p, &bits, sizeof(bits));
}

inline uint32_t LoadU32BE(const uint8_t* p) {
    uint32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return __builtin_bswap32(value);
}

inline int16_t LoadS16BE(const uint8_t* p) {
    uint16_t bits = 0;
    std::memcpy(&bits, p, sizeof(bits));
    bits = __builtin_bswap16(bits);
    int16_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void StoreS16BE(uint8_t* p, int16_t value) {
    uint16_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bits = __builtin_bswap16(bits);
    std::memcpy(p, &bits, sizeof(bits));
}

// One component load through a stream's GQR load side.
inline float LoadComponent(const uint8_t* p, ElemKind kind, uint32_t scale) {
    if (kind == ElemKind::kFloat32) {
        return LoadF32BE(p);
    }
    return DequantizeS16(LoadS16BE(p), scale);
}

// One component store through a stream's GQR store side.
inline void StoreComponent(uint8_t* p, float value, ElemKind kind, uint32_t scale) {
    if (kind == ElemKind::kFloat32) {
        StoreF32BE(p, value);
        return;
    }
    StoreS16BE(p, QuantizeS16(value, scale));
}

// Element size in bytes for a stream kind (used by the caller's span math).
inline uint32_t ElemSize(ElemKind kind) {
    return kind == ElemKind::kFloat32 ? 4u : 2u;
}

struct SkinParams {
    const uint8_t* weights = nullptr;  // 16 B per vertex (4 F32)
    const uint8_t* indices = nullptr;  // 4 B per vertex (1 u32)
    const uint8_t* palette = nullptr;  // 64 B per matrix (256 bones max)
    const uint8_t* inPos = nullptr;    // position input stream
    const uint8_t* inNrm = nullptr;    // normal input stream (unused if !hasNormals)
    uint8_t* outPos = nullptr;         // position output stream
    uint8_t* outNrm = nullptr;         // normal output stream (unused if !hasNormals)
    int32_t count = 0;                 // vertex count; must be > 0 (caller handles <= 0)
    uint32_t posStride = 0;            // bytes between position components (in and out)
    uint32_t nrmStride = 0;            // bytes between normal components (in and out)
    uint32_t nrmExtra = 0;             // extra bytes added to normal streams per vertex
    ElemKind posLoadKind = ElemKind::kFloat32;
    uint32_t posLoadScale = 0;
    ElemKind posStoreKind = ElemKind::kFloat32;
    uint32_t posStoreScale = 0;
    ElemKind nrmLoadKind = ElemKind::kFloat32;
    uint32_t nrmLoadScale = 0;
    ElemKind nrmStoreKind = ElemKind::kFloat32;
    uint32_t nrmStoreScale = 0;
    bool hasNormals = false;
};

#if defined(__aarch64__)

// One bone's transform, NEON. m0..m3 are the matrix rows (float32x4_t, lanes
// 0-2 are the used columns, lane 3 is the unused 4th column and is never
// stored). Returns the position (px, py, pz) in lanes 0-2 and the normal
// (nx, ny, nz) in lanes 0-2. The per-lane FMA order matches the guest's
// paired-single fmadd sequence exactly; lanes are independent.
inline void BoneTransformNeon(float32x4_t m0, float32x4_t m1, float32x4_t m2, float32x4_t m3,
                              float x, float y, float z, float nx, float ny, float nz,
                              float32x4_t& outPos, float32x4_t& outNrm) {
    // Position: seed is the translation row, then FMA x, y, z in order.
    float32x4_t pos = vfmaq_f32(m3, m0, vdupq_n_f32(x));
    pos = vfmaq_f32(pos, m1, vdupq_n_f32(y));
    pos = vfmaq_f32(pos, m2, vdupq_n_f32(z));
    // Normal: seed is a plain multiply (no translation), then FMA y, z.
    float32x4_t nrm = vmulq_f32(m0, vdupq_n_f32(nx));
    nrm = vfmaq_f32(nrm, m1, vdupq_n_f32(ny));
    nrm = vfmaq_f32(nrm, m2, vdupq_n_f32(nz));
    outPos = pos;
    outNrm = nrm;
}

// Load one matrix row (4 F32, guest order) as a NEON vector. The byte
// reversal is the guest-to-host conversion; lane 3 is the unused column.
inline float32x4_t LoadMatrixRowNeon(const uint8_t* p) {
    const uint8x16_t raw = vld1q_u8(p);
    const uint8x16_t swapped = vrev32q_u8(raw);
    return vreinterpretq_f32_u8(swapped);
}

inline void SkinCore(const SkinParams& p) {
    const uint32_t posVertexStride = 3u * p.posStride;
    const uint32_t nrmVertexStride = 3u * p.nrmStride + p.nrmExtra;
    for (int32_t i = 0; i < p.count; ++i) {
        // Weights (4 F32) and the packed bone-index word.
        const uint8_t* wPtr = p.weights + static_cast<size_t>(i) * 16u;
        const float w0 = LoadF32BE(wPtr);
        const float w1 = LoadF32BE(wPtr + 4);
        const float w2 = LoadF32BE(wPtr + 8);
        const float w3 = LoadF32BE(wPtr + 12);
        const uint32_t word = LoadU32BE(p.indices + static_cast<size_t>(i) * 4u);
        const uint32_t b0 = word & 0xFFu;
        const uint32_t b1 = (word >> 8) & 0xFFu;
        const uint32_t b2 = (word >> 16) & 0xFFu;
        const uint32_t b3 = (word >> 24) & 0xFFu;

        // Position and (when present) normal inputs.
        const uint8_t* inPos = p.inPos + static_cast<size_t>(i) * posVertexStride;
        const float x = LoadComponent(inPos, p.posLoadKind, p.posLoadScale);
        const float y = LoadComponent(inPos + p.posStride, p.posLoadKind, p.posLoadScale);
        const float z = LoadComponent(inPos + 2u * p.posStride, p.posLoadKind, p.posLoadScale);
        float nx = 0.0f;
        float ny = 0.0f;
        float nz = 0.0f;
        const uint8_t* inNrm = nullptr;
        if (p.hasNormals) {
            inNrm = p.inNrm + static_cast<size_t>(i) * nrmVertexStride;
            nx = LoadComponent(inNrm, p.nrmLoadKind, p.nrmLoadScale);
            ny = LoadComponent(inNrm + p.nrmStride, p.nrmLoadKind, p.nrmLoadScale);
            nz = LoadComponent(inNrm + 2u * p.nrmStride, p.nrmLoadKind, p.nrmLoadScale);
        }
        (void)inNrm;

        // Bone 0 always; bone j only when all of w1..wj are > 0 (chained skip).
        const uint8_t* mat0 = p.palette + static_cast<size_t>(b0) * 64u;
        float32x4_t m0 = LoadMatrixRowNeon(mat0);
        float32x4_t m1 = LoadMatrixRowNeon(mat0 + 16);
        float32x4_t m2 = LoadMatrixRowNeon(mat0 + 32);
        float32x4_t m3 = LoadMatrixRowNeon(mat0 + 48);
        float32x4_t accPos = {};
        float32x4_t accNrm = {};
        {
            float32x4_t bonePos = {};
            float32x4_t boneNrm = {};
            BoneTransformNeon(m0, m1, m2, m3, x, y, z, nx, ny, nz, bonePos, boneNrm);
            accPos = vmulq_f32(bonePos, vdupq_n_f32(w0));
            accNrm = vmulq_f32(boneNrm, vdupq_n_f32(w0));
        }
        if (w1 > 0.0f) {
            const uint8_t* mat1 = p.palette + static_cast<size_t>(b1) * 64u;
            m0 = LoadMatrixRowNeon(mat1);
            m1 = LoadMatrixRowNeon(mat1 + 16);
            m2 = LoadMatrixRowNeon(mat1 + 32);
            m3 = LoadMatrixRowNeon(mat1 + 48);
            float32x4_t bonePos = {};
            float32x4_t boneNrm = {};
            BoneTransformNeon(m0, m1, m2, m3, x, y, z, nx, ny, nz, bonePos, boneNrm);
            accPos = vfmaq_f32(accPos, bonePos, vdupq_n_f32(w1));
            accNrm = vfmaq_f32(accNrm, boneNrm, vdupq_n_f32(w1));
            if (w2 > 0.0f) {
                const uint8_t* mat2 = p.palette + static_cast<size_t>(b2) * 64u;
                m0 = LoadMatrixRowNeon(mat2);
                m1 = LoadMatrixRowNeon(mat2 + 16);
                m2 = LoadMatrixRowNeon(mat2 + 32);
                m3 = LoadMatrixRowNeon(mat2 + 48);
                BoneTransformNeon(m0, m1, m2, m3, x, y, z, nx, ny, nz, bonePos, boneNrm);
                accPos = vfmaq_f32(accPos, bonePos, vdupq_n_f32(w2));
                accNrm = vfmaq_f32(accNrm, boneNrm, vdupq_n_f32(w2));
                if (w3 > 0.0f) {
                    const uint8_t* mat3 = p.palette + static_cast<size_t>(b3) * 64u;
                    m0 = LoadMatrixRowNeon(mat3);
                    m1 = LoadMatrixRowNeon(mat3 + 16);
                    m2 = LoadMatrixRowNeon(mat3 + 32);
                    m3 = LoadMatrixRowNeon(mat3 + 48);
                    BoneTransformNeon(m0, m1, m2, m3, x, y, z, nx, ny, nz, bonePos, boneNrm);
                    accPos = vfmaq_f32(accPos, bonePos, vdupq_n_f32(w3));
                    accNrm = vfmaq_f32(accNrm, boneNrm, vdupq_n_f32(w3));
                }
            }
        }

        // Stores: lanes 0-2 only; the 4th lane is the unused matrix column.
        uint8_t* outPos = p.outPos + static_cast<size_t>(i) * posVertexStride;
        StoreComponent(outPos, vgetq_lane_f32(accPos, 0), p.posStoreKind, p.posStoreScale);
        StoreComponent(outPos + p.posStride, vgetq_lane_f32(accPos, 1), p.posStoreKind, p.posStoreScale);
        StoreComponent(outPos + 2u * p.posStride, vgetq_lane_f32(accPos, 2), p.posStoreKind, p.posStoreScale);
        if (p.hasNormals) {
            uint8_t* outNrm = p.outNrm + static_cast<size_t>(i) * nrmVertexStride;
            StoreComponent(outNrm, vgetq_lane_f32(accNrm, 0), p.nrmStoreKind, p.nrmStoreScale);
            StoreComponent(outNrm + p.nrmStride, vgetq_lane_f32(accNrm, 1), p.nrmStoreKind, p.nrmStoreScale);
            StoreComponent(outNrm + 2u * p.nrmStride, vgetq_lane_f32(accNrm, 2), p.nrmStoreKind, p.nrmStoreScale);
        }
    }
}

#else  // not __aarch64__: scalar fallback (same float order, std::fma).
// The fallback exists so the differential test and developer builds compile
// on hosts without NEON. It is the reference algorithm; the aarch64 NEON
// path above is the production path, and the test proves them bit-identical.
inline void SkinCore(const SkinParams& p) {
    const uint32_t posVertexStride = 3u * p.posStride;
    const uint32_t nrmVertexStride = 3u * p.nrmStride + p.nrmExtra;
    for (int32_t i = 0; i < p.count; ++i) {
        const uint8_t* wPtr = p.weights + static_cast<size_t>(i) * 16u;
        const float w0 = LoadF32BE(wPtr);
        const float w1 = LoadF32BE(wPtr + 4);
        const float w2 = LoadF32BE(wPtr + 8);
        const float w3 = LoadF32BE(wPtr + 12);
        const uint32_t word = LoadU32BE(p.indices + static_cast<size_t>(i) * 4u);
        const uint32_t bones[4] = {word & 0xFFu, (word >> 8) & 0xFFu, (word >> 16) & 0xFFu,
                                   (word >> 24) & 0xFFu};
        const uint8_t* inPos = p.inPos + static_cast<size_t>(i) * posVertexStride;
        const float x = LoadComponent(inPos, p.posLoadKind, p.posLoadScale);
        const float y = LoadComponent(inPos + p.posStride, p.posLoadKind, p.posLoadScale);
        const float z = LoadComponent(inPos + 2u * p.posStride, p.posLoadKind, p.posLoadScale);
        float nx = 0.0f;
        float ny = 0.0f;
        float nz = 0.0f;
        if (p.hasNormals) {
            const uint8_t* inNrm = p.inNrm + static_cast<size_t>(i) * nrmVertexStride;
            nx = LoadComponent(inNrm, p.nrmLoadKind, p.nrmLoadScale);
            ny = LoadComponent(inNrm + p.nrmStride, p.nrmLoadKind, p.nrmLoadScale);
            nz = LoadComponent(inNrm + 2u * p.nrmStride, p.nrmLoadKind, p.nrmLoadScale);
        }
        const float weights[4] = {w0, w1, w2, w3};
        float accPx = 0.0f;
        float accPy = 0.0f;
        float accPz = 0.0f;
        float accNx = 0.0f;
        float accNy = 0.0f;
        float accNz = 0.0f;
        // Bone 0 always; bone j only when all of w1..wj are > 0.
        int bonesToDo = 1;
        if (w1 > 0.0f) {
            bonesToDo = 2;
            if (w2 > 0.0f) {
                bonesToDo = 3;
                if (w3 > 0.0f) {
                    bonesToDo = 4;
                }
            }
        }
        for (int j = 0; j < bonesToDo; ++j) {
            const uint8_t* mat = p.palette + static_cast<size_t>(bones[j]) * 64u;
            const float m0 = LoadF32BE(mat);
            const float m1 = LoadF32BE(mat + 4);
            const float m2 = LoadF32BE(mat + 8);
            const float m4 = LoadF32BE(mat + 16);
            const float m5 = LoadF32BE(mat + 20);
            const float m6 = LoadF32BE(mat + 24);
            const float m8 = LoadF32BE(mat + 32);
            const float m9 = LoadF32BE(mat + 36);
            const float m10 = LoadF32BE(mat + 40);
            const float m12 = LoadF32BE(mat + 48);
            const float m13 = LoadF32BE(mat + 52);
            const float m14 = LoadF32BE(mat + 56);
            const float px = std::fma(m8, z, std::fma(m4, y, std::fma(m0, x, m12)));
            const float py = std::fma(m9, z, std::fma(m5, y, std::fma(m1, x, m13)));
            const float pz = std::fma(m10, z, std::fma(m6, y, std::fma(m2, x, m14)));
            const float nxx = std::fma(m8, nz, std::fma(m4, ny, m0 * nx));
            const float nyy = std::fma(m9, nz, std::fma(m5, ny, m1 * nx));
            const float nzz = std::fma(m10, nz, std::fma(m6, ny, m2 * nx));
            const float wj = weights[j];
            if (j == 0) {
                accPx = px * wj;
                accPy = py * wj;
                accPz = pz * wj;
                accNx = nxx * wj;
                accNy = nyy * wj;
                accNz = nzz * wj;
            } else {
                accPx = std::fma(px, wj, accPx);
                accPy = std::fma(py, wj, accPy);
                accPz = std::fma(pz, wj, accPz);
                accNx = std::fma(nxx, wj, accNx);
                accNy = std::fma(nyy, wj, accNy);
                accNz = std::fma(nzz, wj, accNz);
            }
        }
        uint8_t* outPos = p.outPos + static_cast<size_t>(i) * posVertexStride;
        StoreComponent(outPos, accPx, p.posStoreKind, p.posStoreScale);
        StoreComponent(outPos + p.posStride, accPy, p.posStoreKind, p.posStoreScale);
        StoreComponent(outPos + 2u * p.posStride, accPz, p.posStoreKind, p.posStoreScale);
        if (p.hasNormals) {
            uint8_t* outNrm = p.outNrm + static_cast<size_t>(i) * nrmVertexStride;
            StoreComponent(outNrm, accNx, p.nrmStoreKind, p.nrmStoreScale);
            StoreComponent(outNrm + p.nrmStride, accNy, p.nrmStoreKind, p.nrmStoreScale);
            StoreComponent(outNrm + 2u * p.nrmStride, accNz, p.nrmStoreKind, p.nrmStoreScale);
        }
    }
}

#endif  // __aarch64__

}  // namespace No100fRpSkin
