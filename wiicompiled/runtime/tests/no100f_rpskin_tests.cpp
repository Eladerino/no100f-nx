// NO100F: differential test for the RpSkin native (func_80144E8C).
//
// The native (include/hle/no100f_rpskin.h, NEON on aarch64) is compared
// bit-for-bit against a scalar reference written independently from the
// generated kernel body (shard_d08f2756ebb3fe397e55be6c.cpp:29326). Both run
// on the same guest-order (big-endian) buffers; only the outputs are
// compared (memcmp), so any difference in float order, control flow, lane
// mapping, GQR handling, or byte order fails the test.
//
// Coverage:
//   - Randomized geometries: counts 1..512, normalized and unnormalized
//     weights, bone indices 0..15, matrices/positions/normals across
//     [-10, 10], F32 and S16 streams (scales 0, 1, 8, 15), strides 4/2 and
//     larger interleaved strides, nrmExtra 0 and 4, hasNormals on/off.
//   - Edge cases: weights 0, -0.0, negative, NaN, infinities, denormals;
//     non-sorted weights (w1 = 0 with w2 > 0; w1 > 0, w2 = 0, w3 > 0;
//     w1 < 0 with w2 > 0) which distinguish the chained skip from the
//     zero-weight collapse; positions/normals/matrices with 0, -0.0, NaN,
//     infinities, denormals, and values near the float extremes.
//   - The whole suite runs twice: once with the host flush-to-zero mode on
//     (guest FPSCR.NI set) and once with it off (NI clear). The native must
//     match the reference under both, because the game's NI setting is not
//     pinned by desk work and both sides see the same host FP environment.
//
// Usage: no100f_rpskin_tests [seed] [random-cases]
// Exit 0 when every case matches bit-for-bit, 1 otherwise. The first
// mismatch prints the case, vertex, component, and the two bit patterns.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "hle/no100f_rpskin.h"

namespace {

using namespace No100fRpSkin;

// Host flush-to-zero on/off, matching RecompApplyHostNiMode (FPCR.FZ on
// AArch64, MXCSR FTZ+DAZ on x86_64).
void SetFlushToZero(bool on) {
#if defined(__aarch64__)
    uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    if (on) {
        fpcr |= (1u << 24);
    } else {
        fpcr &= ~(1u << 24);
    }
    __asm__ __volatile__("msr fpcr, %0" ::"r"(fpcr));
#elif defined(__x86_64__)
    unsigned int mxcsr = 0;
    __asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr));
    if (on) {
        mxcsr |= (1u << 15) | (1u << 6);
    } else {
        mxcsr &= ~((1u << 15) | (1u << 6));
    }
    __asm__ __volatile__("ldmxcsr %0" ::"m"(mxcsr));
#else
    (void)on;
#endif
}

// Scalar reference, written from the generated kernel body. It mirrors the
// guest's paired-single structure: the position is two accumulators (x', y')
// in one pair and (z', *) in another; the normal the same. Each lane is one
// FMA chain; bone 0 is weighted by a multiply, bones 1-3 by FMA; a bone is
// evaluated only when all of w1..wj are > 0 (the chained skip).
void SkinReference(const SkinParams& p) {
    const uint32_t posVertexStride = 3u * p.posStride;
    const uint32_t nrmVertexStride = 3u * p.nrmStride + p.nrmExtra;
    for (int32_t i = 0; i < p.count; ++i) {
        const uint8_t* wPtr = p.weights + static_cast<size_t>(i) * 16u;
        const float w0 = LoadF32BE(wPtr);
        const float w1 = LoadF32BE(wPtr + 4);
        const float w2 = LoadF32BE(wPtr + 8);
        const float w3 = LoadF32BE(wPtr + 12);
        const uint32_t word = LoadU32BE(p.indices + static_cast<size_t>(i) * 4u);
        const uint32_t bone[4] = {word & 0xFFu, (word >> 8) & 0xFFu, (word >> 16) & 0xFFu,
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
        // Paired accumulators, as in the guest: (x', y') and (z', *).
        float accXY0 = 0.0f;
        float accXY1 = 0.0f;
        float accZ0 = 0.0f;
        float nrmXY0 = 0.0f;
        float nrmXY1 = 0.0f;
        float nrmZ0 = 0.0f;
        const float w[4] = {w0, w1, w2, w3};
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
            const uint8_t* mat = p.palette + static_cast<size_t>(bone[j]) * 64u;
            // Row 0, 1, 2, 3 of the 3x4 submatrix (cols 0-2 used).
            const float r0c0 = LoadF32BE(mat);
            const float r0c1 = LoadF32BE(mat + 4);
            const float r0c2 = LoadF32BE(mat + 8);
            const float r1c0 = LoadF32BE(mat + 16);
            const float r1c1 = LoadF32BE(mat + 20);
            const float r1c2 = LoadF32BE(mat + 24);
            const float r2c0 = LoadF32BE(mat + 32);
            const float r2c1 = LoadF32BE(mat + 36);
            const float r2c2 = LoadF32BE(mat + 40);
            const float r3c0 = LoadF32BE(mat + 48);
            const float r3c1 = LoadF32BE(mat + 52);
            const float r3c2 = LoadF32BE(mat + 56);
            // Position pair (x', y'): seed is the translation, then x, y, z.
            const float px = std::fma(r2c0, z, std::fma(r1c0, y, std::fma(r0c0, x, r3c0)));
            const float py = std::fma(r2c1, z, std::fma(r1c1, y, std::fma(r0c1, x, r3c1)));
            // Position z': seed is the translation, then x, y, z.
            const float pz = std::fma(r2c2, z, std::fma(r1c2, y, std::fma(r0c2, x, r3c2)));
            // Normal pair (nx', ny'): seed is a multiply, then y, z.
            const float nxx = std::fma(r2c0, nz, std::fma(r1c0, ny, r0c0 * nx));
            const float nyy = std::fma(r2c1, nz, std::fma(r1c1, ny, r0c1 * nx));
            const float nzz = std::fma(r2c2, nz, std::fma(r1c2, ny, r0c2 * nx));
            if (j == 0) {
                accXY0 = px * w[0];
                accXY1 = py * w[0];
                accZ0 = pz * w[0];
                nrmXY0 = nxx * w[0];
                nrmXY1 = nyy * w[0];
                nrmZ0 = nzz * w[0];
            } else {
                accXY0 = std::fma(px, w[j], accXY0);
                accXY1 = std::fma(py, w[j], accXY1);
                accZ0 = std::fma(pz, w[j], accZ0);
                nrmXY0 = std::fma(nxx, w[j], nrmXY0);
                nrmXY1 = std::fma(nyy, w[j], nrmXY1);
                nrmZ0 = std::fma(nzz, w[j], nrmZ0);
            }
        }
        uint8_t* outPos = p.outPos + static_cast<size_t>(i) * posVertexStride;
        StoreComponent(outPos, accXY0, p.posStoreKind, p.posStoreScale);
        StoreComponent(outPos + p.posStride, accXY1, p.posStoreKind, p.posStoreScale);
        StoreComponent(outPos + 2u * p.posStride, accZ0, p.posStoreKind, p.posStoreScale);
        if (p.hasNormals) {
            uint8_t* outNrm = p.outNrm + static_cast<size_t>(i) * nrmVertexStride;
            StoreComponent(outNrm, nrmXY0, p.nrmStoreKind, p.nrmStoreScale);
            StoreComponent(outNrm + p.nrmStride, nrmXY1, p.nrmStoreKind, p.nrmStoreScale);
            StoreComponent(outNrm + 2u * p.nrmStride, nrmZ0, p.nrmStoreKind, p.nrmStoreScale);
        }
    }
}

// Write a float into a guest-order buffer.
void PutF32BE(uint8_t* p, float value) {
    StoreF32BE(p, value);
}

void PutU32BE(uint8_t* p, uint32_t value) {
    uint32_t swapped = __builtin_bswap32(value);
    std::memcpy(p, &swapped, sizeof(swapped));
}

void PutS16BE(uint8_t* p, int16_t value) {
    StoreS16BE(p, value);
}

uint32_t FloatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct CaseBuffers {
    std::vector<uint8_t> weights;
    std::vector<uint8_t> indices;
    std::vector<uint8_t> palette;
    std::vector<uint8_t> inPos;
    std::vector<uint8_t> inNrm;
    std::vector<uint8_t> outPosA;
    std::vector<uint8_t> outNrmA;
    std::vector<uint8_t> outPosB;
    std::vector<uint8_t> outNrmB;
};

int g_failures = 0;
int g_cases = 0;

void CheckCase(const char* name, const SkinParams& base, CaseBuffers& b) {
    ++g_cases;
    SkinParams pa = base;
    SkinParams pb = base;
    pa.weights = b.weights.data();
    pa.indices = b.indices.data();
    pa.palette = b.palette.data();
    pa.inPos = b.inPos.data();
    pa.inNrm = b.inNrm.data();
    pa.outPos = b.outPosA.data();
    pa.outNrm = b.outNrmA.data();
    pb = pa;
    pb.outPos = b.outPosB.data();
    pb.outNrm = b.outNrmB.data();
    SkinCore(pa);
    SkinReference(pb);
    if (std::memcmp(b.outPosA.data(), b.outPosB.data(), b.outPosA.size()) != 0) {
        ++g_failures;
        std::printf("FAIL %s: outPos differs\n", name);
        for (size_t k = 0; k < b.outPosA.size(); ++k) {
            if (b.outPosA[k] != b.outPosB[k]) {
                std::printf("  first byte diff at offset %zu: native 0x%02x ref 0x%02x\n", k,
                            b.outPosA[k], b.outPosB[k]);
                break;
            }
        }
        return;
    }
    if (base.hasNormals &&
        std::memcmp(b.outNrmA.data(), b.outNrmB.data(), b.outNrmA.size()) != 0) {
        ++g_failures;
        std::printf("FAIL %s: outNrm differs\n", name);
        for (size_t k = 0; k < b.outNrmA.size(); ++k) {
            if (b.outNrmA[k] != b.outNrmB[k]) {
                std::printf("  first byte diff at offset %zu: native 0x%02x ref 0x%02x\n", k,
                            b.outNrmA[k], b.outNrmB[k]);
                break;
            }
        }
        return;
    }
}

// Fill a stream (3 components per vertex at compStride) from float values,
// converting through the stream's load kind/scale.
void FillStream(std::vector<uint8_t>& buf, int32_t count, uint32_t compStride, uint32_t extra,
                ElemKind kind, uint32_t scale, const std::vector<float>& values) {
    const uint32_t vertexStride = 3u * compStride + extra;
    for (int32_t i = 0; i < count; ++i) {
        for (int c = 0; c < 3; ++c) {
            uint8_t* dst = buf.data() + static_cast<size_t>(i) * vertexStride + c * compStride;
            const float v = values[static_cast<size_t>(i) * 3u + c];
            if (kind == ElemKind::kFloat32) {
                PutF32BE(dst, v);
            } else {
                PutS16BE(dst, QuantizeS16(v, scale));
            }
        }
    }
}

SkinParams BaseParams(int32_t count, bool hasNormals, ElemKind posKind, uint32_t posScale,
                      ElemKind nrmKind, uint32_t nrmScale, uint32_t posStride, uint32_t nrmStride,
                      uint32_t nrmExtra) {
    SkinParams p;
    p.count = count;
    p.hasNormals = hasNormals;
    p.posLoadKind = posKind;
    p.posLoadScale = posScale;
    p.posStoreKind = posKind;
    p.posStoreScale = posScale;
    p.nrmLoadKind = nrmKind;
    p.nrmLoadScale = nrmScale;
    p.nrmStoreKind = nrmKind;
    p.nrmStoreScale = nrmScale;
    p.posStride = posStride;
    p.nrmStride = nrmStride;
    p.nrmExtra = nrmExtra;
    return p;
}

void RunEdgeCases() {
    // Weights that distinguish the chained skip from the collapse, one vertex
    // each, identity-ish matrices (bone j scales by 1+j so a wrongly included
    // bone changes the output), position (1, 2, 3), normal (0, 1, 0).
    const float kEdgeWeights[][4] = {
        {1.0f, 0.0f, 0.5f, 0.0f},    // w1 = 0, w2 > 0: skip must drop bone 2
        {1.0f, 0.5f, 0.0f, 0.5f},    // w2 = 0, w3 > 0: skip must drop bone 3
        {1.0f, -0.5f, 0.5f, 0.5f},   // w1 < 0: skip must drop bones 1-3
        {1.0f, 0.0f, 0.0f, 0.0f},    // only bone 0
        {0.0f, 0.5f, 0.5f, 0.5f},    // w0 = 0 (bone 0 still evaluated, times 0)
        {1.0f, NAN, 0.5f, 0.5f},     // w1 = NaN: not > 0, skip bones 1-3
        {1.0f, -0.0f, 0.5f, 0.5f},   // w1 = -0.0: not > 0, skip
        {1.0f, 0.5f, NAN, 0.5f},     // w2 = NaN: skip bones 2-3
        {1.0f, 0.5f, 0.5f, INFINITY},  // w3 = +inf: bone 3 included
        {1.0f, 0.5f, 0.5f, -INFINITY}, // w3 = -inf: not > 0, skip bone 3
        {0.25f, 0.25f, 0.25f, 0.25f},  // all four, normalized
        {1.0f, 1.0f, 1.0f, 1.0f},      // all four, unnormalized
    };
    for (size_t t = 0; t < sizeof(kEdgeWeights) / sizeof(kEdgeWeights[0]); ++t) {
        CaseBuffers b;
        b.weights.resize(16);
        for (int k = 0; k < 4; ++k) PutF32BE(b.weights.data() + k * 4, kEdgeWeights[t][k]);
        b.indices.resize(4);
        PutU32BE(b.indices.data(), 0x03020100u);  // bones 0,1,2,3
        b.palette.resize(4 * 64);
        for (int bone = 0; bone < 4; ++bone) {
            for (int k = 0; k < 16; ++k) {
                // Bone j: scale (1+j) on the diagonal, translation j.
                float v = 0.0f;
                if (k == 0 || k == 5 || k == 10) v = 1.0f + static_cast<float>(bone);
                if (k == 12) v = static_cast<float>(bone);
                if (k == 13) v = static_cast<float>(bone) * 2.0f;
                if (k == 14) v = static_cast<float>(bone) * 3.0f;
                PutF32BE(b.palette.data() + bone * 64 + k * 4, v);
            }
        }
        b.inPos.resize(12);
        PutF32BE(b.inPos.data(), 1.0f);
        PutF32BE(b.inPos.data() + 4, 2.0f);
        PutF32BE(b.inPos.data() + 8, 3.0f);
        b.inNrm.resize(12);
        PutF32BE(b.inNrm.data(), 0.0f);
        PutF32BE(b.inNrm.data() + 4, 1.0f);
        PutF32BE(b.inNrm.data() + 8, 0.0f);
        b.outPosA.assign(12, 0xA5);
        b.outNrmA.assign(12, 0xA5);
        b.outPosB.assign(12, 0xA5);
        b.outNrmB.assign(12, 0xA5);
        char name[64];
        std::snprintf(name, sizeof(name), "edge-weights-%zu", t);
        SkinParams p = BaseParams(1, true, ElemKind::kFloat32, 0, ElemKind::kFloat32, 0, 4, 4, 0);
        CheckCase(name, p, b);
    }

    // Extreme values in positions, normals, and matrices.
    const float kExtreme[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e-30f, -1e-30f, 1e30f, -1e30f,
                              3.4028235e38f, -3.4028235e38f, INFINITY, -INFINITY, NAN,
                              1.1754944e-38f, 1.4012985e-45f, -1.4012985e-45f};
    for (size_t t = 0; t < sizeof(kExtreme) / sizeof(kExtreme[0]); ++t) {
        CaseBuffers b;
        b.weights.resize(16);
        PutF32BE(b.weights.data(), 0.6f);
        PutF32BE(b.weights.data() + 4, 0.4f);
        PutF32BE(b.weights.data() + 8, 0.0f);
        PutF32BE(b.weights.data() + 12, 0.0f);
        b.indices.resize(4);
        PutU32BE(b.indices.data(), 0x00000100u);  // bones 0,1
        b.palette.resize(2 * 64);
        for (int bone = 0; bone < 2; ++bone) {
            for (int k = 0; k < 16; ++k) {
                float v = (k % 5 == 0) ? 1.0f : 0.0f;
                if (k == 0) v = kExtreme[t];  // m00: a USED element, so the extreme flows in
                PutF32BE(b.palette.data() + bone * 64 + k * 4, v);
            }
        }
        b.inPos.resize(12);
        PutF32BE(b.inPos.data(), kExtreme[t]);
        PutF32BE(b.inPos.data() + 4, 1.0f);
        PutF32BE(b.inPos.data() + 8, -1.0f);
        b.inNrm.resize(12);
        PutF32BE(b.inNrm.data(), 0.5f);
        PutF32BE(b.inNrm.data() + 4, kExtreme[t]);
        PutF32BE(b.inNrm.data() + 8, 0.25f);
        b.outPosA.assign(12, 0xA5);
        b.outNrmA.assign(12, 0xA5);
        b.outPosB.assign(12, 0xA5);
        b.outNrmB.assign(12, 0xA5);
        char name[64];
        std::snprintf(name, sizeof(name), "edge-extreme-%zu", t);
        SkinParams p = BaseParams(1, true, ElemKind::kFloat32, 0, ElemKind::kFloat32, 0, 4, 4, 0);
        CheckCase(name, p, b);
    }

    // No-normals case and S16 streams at several scales.
    {
        CaseBuffers b;
        b.weights.resize(32);
        for (int v = 0; v < 2; ++v) {
            PutF32BE(b.weights.data() + v * 16, 0.7f);
            PutF32BE(b.weights.data() + v * 16 + 4, 0.3f);
            PutF32BE(b.weights.data() + v * 16 + 8, 0.0f);
            PutF32BE(b.weights.data() + v * 16 + 12, 0.0f);
        }
        b.indices.resize(8);
        PutU32BE(b.indices.data(), 0x00000100u);
        PutU32BE(b.indices.data() + 4, 0x00010000u);
        b.palette.resize(2 * 64);
        for (int bone = 0; bone < 2; ++bone) {
            for (int k = 0; k < 16; ++k) {
                PutF32BE(b.palette.data() + bone * 64 + k * 4, (k % 5 == 0) ? 1.0f : 0.0f);
            }
        }
        b.inPos.resize(24);
        for (int v = 0; v < 2; ++v) {
            PutF32BE(b.inPos.data() + v * 12, 1.0f + v);
            PutF32BE(b.inPos.data() + v * 12 + 4, 2.0f + v);
            PutF32BE(b.inPos.data() + v * 12 + 8, 3.0f + v);
        }
        b.outPosA.assign(24, 0xA5);
        b.outNrmA.assign(24, 0xA5);
        b.outPosB.assign(24, 0xA5);
        b.outNrmB.assign(24, 0xA5);
        SkinParams p = BaseParams(2, false, ElemKind::kFloat32, 0, ElemKind::kFloat32, 0, 4, 4, 0);
        CheckCase("edge-no-normals", p, b);
    }
    const uint32_t kScales[] = {0u, 1u, 8u, 15u};
    for (size_t s = 0; s < sizeof(kScales) / sizeof(kScales[0]); ++s) {
        CaseBuffers b;
        b.weights.resize(16);
        PutF32BE(b.weights.data(), 0.6f);
        PutF32BE(b.weights.data() + 4, 0.4f);
        PutF32BE(b.weights.data() + 8, 0.0f);
        PutF32BE(b.weights.data() + 12, 0.0f);
        b.indices.resize(4);
        PutU32BE(b.indices.data(), 0x00000100u);
        b.palette.resize(2 * 64);
        for (int bone = 0; bone < 2; ++bone) {
            for (int k = 0; k < 16; ++k) {
                PutF32BE(b.palette.data() + bone * 64 + k * 4, (k % 5 == 0) ? 1.0f : 0.0f);
            }
        }
        // S16 positions/normals: values that survive the round trip.
        b.inPos.resize(6);
        b.inNrm.resize(6);
        const float pos[3] = {0.5f, -0.25f, 0.125f};
        const float nrm[3] = {0.0f, 1.0f, 0.0f};
        for (int c = 0; c < 3; ++c) {
            PutS16BE(b.inPos.data() + c * 2, QuantizeS16(pos[c], kScales[s]));
            PutS16BE(b.inNrm.data() + c * 2, QuantizeS16(nrm[c], kScales[s]));
        }
        b.outPosA.assign(6, 0xA5);
        b.outNrmA.assign(6, 0xA5);
        b.outPosB.assign(6, 0xA5);
        b.outNrmB.assign(6, 0xA5);
        char name[64];
        std::snprintf(name, sizeof(name), "edge-s16-scale-%u", kScales[s]);
        SkinParams p = BaseParams(1, true, ElemKind::kS16, kScales[s], ElemKind::kS16, kScales[s],
                                  2, 2, 0);
        CheckCase(name, p, b);
    }
}

void RunRandomCases(int count, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::uniform_real_distribution<float> wide(-10.0f, 10.0f);
    std::uniform_real_distribution<float> mat(-2.0f, 2.0f);
    std::uniform_int_distribution<int> boneIdx(0, 15);
    std::uniform_int_distribution<int> vertexCount(1, 512);
    std::uniform_int_distribution<int> boolDist(0, 1);
    const uint32_t kS16Scales[] = {0u, 1u, 4u, 8u, 12u, 15u};
    for (int t = 0; t < count; ++t) {
        const int32_t n = vertexCount(rng);
        const bool hasNormals = boolDist(rng) != 0;
        const bool posS16 = boolDist(rng) != 0;
        const bool nrmS16 = boolDist(rng) != 0;
        const uint32_t posScale = kS16Scales[rng() % 6];
        const uint32_t nrmScale = kS16Scales[rng() % 6];
        const ElemKind posKind = posS16 ? ElemKind::kS16 : ElemKind::kFloat32;
        const ElemKind nrmKind = nrmS16 ? ElemKind::kS16 : ElemKind::kFloat32;
        // Strides: tight, or interleaved with a gap.
        const uint32_t posStride = posS16 ? (boolDist(rng) ? 2u : 8u) : (boolDist(rng) ? 4u : 16u);
        const uint32_t nrmStride = nrmS16 ? (boolDist(rng) ? 2u : 8u) : (boolDist(rng) ? 4u : 16u);
        const uint32_t nrmExtra = (boolDist(rng) != 0) ? 4u : 0u;
        CaseBuffers b;
        b.weights.resize(static_cast<size_t>(n) * 16u);
        b.indices.resize(static_cast<size_t>(n) * 4u);
        b.palette.resize(16 * 64);
        std::vector<float> posVals(static_cast<size_t>(n) * 3u);
        std::vector<float> nrmVals(static_cast<size_t>(n) * 3u);
        for (int32_t i = 0; i < n; ++i) {
            // Weights: mostly normalized, sometimes unnormalized, sometimes
            // with a zero or negative in a later slot.
            float w[4] = {unit(rng) * 0.5f + 0.5f, unit(rng) * 0.5f + 0.5f, unit(rng) * 0.5f + 0.5f,
                          unit(rng) * 0.5f + 0.5f};
            const float sum = w[0] + w[1] + w[2] + w[3];
            if (sum > 0.0f && boolDist(rng) != 0) {
                for (int k = 0; k < 4; ++k) w[k] /= sum;
            }
            if (boolDist(rng) != 0) w[1 + (rng() % 3)] = 0.0f;
            if (boolDist(rng) != 0) w[1 + (rng() % 3)] = -unit(rng);
            for (int k = 0; k < 4; ++k) PutF32BE(b.weights.data() + i * 16 + k * 4, w[k]);
            const uint32_t word = static_cast<uint32_t>(boneIdx(rng)) |
                                  (static_cast<uint32_t>(boneIdx(rng)) << 8) |
                                  (static_cast<uint32_t>(boneIdx(rng)) << 16) |
                                  (static_cast<uint32_t>(boneIdx(rng)) << 24);
            PutU32BE(b.indices.data() + i * 4, word);
            for (int c = 0; c < 3; ++c) {
                posVals[static_cast<size_t>(i) * 3u + c] = wide(rng);
                nrmVals[static_cast<size_t>(i) * 3u + c] = unit(rng);
            }
        }
        for (int bone = 0; bone < 16; ++bone) {
            for (int k = 0; k < 16; ++k) {
                PutF32BE(b.palette.data() + bone * 64 + k * 4, mat(rng));
            }
        }
        b.inPos.assign(static_cast<size_t>(n) * 3u * posStride, 0x5A);
        b.inNrm.assign(static_cast<size_t>(n) * (3u * nrmStride + nrmExtra), 0x5A);
        FillStream(b.inPos, n, posStride, 0, posKind, posScale, posVals);
        FillStream(b.inNrm, n, nrmStride, nrmExtra, nrmKind, nrmScale, nrmVals);
        b.outPosA.assign(static_cast<size_t>(n) * 3u * posStride, 0xA5);
        b.outNrmA.assign(static_cast<size_t>(n) * (3u * nrmStride + nrmExtra), 0xA5);
        b.outPosB = b.outPosA;
        b.outNrmB = b.outNrmA;
        char name[64];
        std::snprintf(name, sizeof(name), "random-%d", t);
        SkinParams p = BaseParams(n, hasNormals, posKind, posScale, nrmKind, nrmScale, posStride,
                                  nrmStride, nrmExtra);
        CheckCase(name, p, b);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const uint32_t seed = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 12345u;
    const int randomCases = argc > 2 ? std::atoi(argv[2]) : 200;
    for (int pass = 0; pass < 2; ++pass) {
        const bool fz = pass == 0;
        SetFlushToZero(fz);
        g_cases = 0;
        g_failures = 0;
        RunEdgeCases();
        RunRandomCases(randomCases, seed);
        std::printf("pass fz=%d: %d cases, %d failures\n", fz ? 1 : 0, g_cases, g_failures);
        if (g_failures != 0) return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
