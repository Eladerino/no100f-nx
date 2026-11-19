// Host bindings for the guest C library's hot entries (build round 1, per
// QUICK-AUDITS.md Audit 2): sprintf, sin and cos carry measured per-frame
// shares in the route profile (~3.1% and ~0.6% of game-thread samples);
// the remaining libm entries ride along at zero marginal cost.
//
// The bindings replace the translated bodies outright (PPC_NATIVE_OVERRIDE
// makes the translator skip the address), so correctness rests on the
// differential tests in tests/no100f_libm_bindings_tests.cpp: the sprintf
// adapter + RuntimeHle::FormatGuestPrintf are compared byte-for-byte
// against host snprintf over the game's own format-string corpus
// (extracted from game/main.dol), and the trig bindings are compared
// against long-double references with a measured ULP bound. Only the
// public entries are bound; __kernel_sin/__kernel_cos/__ieee754_rem_pio2
// are never bound directly (they become unreachable once sin/cos are).

#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"

#include "hle/guest_printf.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace {

// Reads a NUL-terminated guest string, capped like the OSReport reader.
std::string ReadGuestString(uint32_t address) {
    if (address == 0) return {};
    std::string value;
    value.reserve(64);
    for (uint32_t i = 0; i < 4096; ++i) {
        const uint8_t byte = Memory::Read8(address + i);
        if (byte == 0) break;
        value.push_back(static_cast<char>(byte));
    }
    return value;
}

// PPC varargs walk for sprintf(const char*, ...): the fixed arguments are
// out=r3 and format=r4, so varargs begin at r5. Modelled on the OSReport
// varargs state (os_report.cpp) with the base register shifted by two:
// integer arguments walk r5..r10 and then the caller's stack parameter
// area at SP+8; a 64-bit argument must start at an odd register (pairs
// (r5,r6), (r7,r8), (r9,r10) relative to the r3-based parameter list), so
// an even current register is skipped. Floating arguments walk f1..f8 on
// an independent counter, exactly as a PPC va_list does.
class SprintfVarArgState {
public:
    explicit SprintfVarArgState(CpuContext* ctx) : ctx_(ctx) {}

    uint32_t Next32() {
        const uint32_t index = gprIndex_++;
        if (index < kRegisterSlots) return ctx_->gpr[kFirstArgGpr + index];
        return Memory::Read32(ctx_->gpr[1] + 8 + (index - kRegisterSlots) * 4);
    }

    uint64_t Next64() {
        if (((kFirstArgGpr + gprIndex_) & 1u) == 0) ++gprIndex_;  // pair starts at an odd register
        const uint64_t hi = Next32();
        const uint64_t lo = Next32();
        return (hi << 32) | lo;
    }

    double NextDouble() {
        if (fprIndex_ < 8) return ctx_->fpr[1 + fprIndex_++].d;
        ++fprIndex_;
        return 0.0;
    }

private:
    static constexpr uint32_t kFirstArgGpr = 5;    // sprintf varargs start at r5
    static constexpr uint32_t kRegisterSlots = 6;  // r5..r10
    CpuContext* ctx_;
    uint32_t gprIndex_ = 0;
    uint32_t fprIndex_ = 0;
};

}  // namespace

extern "C" void No100f_HleSprintf(CpuContext* ctx) {
    const uint32_t outAddress = ctx->gpr[3];
    const std::string format = ReadGuestString(ctx->gpr[4]);
    SprintfVarArgState args(ctx);
    size_t outChars = 0;
    const std::string rendered = RuntimeHle::FormatGuestPrintf(
        format, outChars,
        [&]() { return args.Next32(); },
        [&]() { return args.Next64(); },
        [&]() { return args.NextDouble(); },
        [&](uint32_t address) { return ReadGuestString(address); });
    for (size_t i = 0; i < rendered.size(); ++i) {
        Memory::Write8(outAddress + static_cast<uint32_t>(i), static_cast<uint8_t>(rendered[i]));
    }
    Memory::Write8(outAddress + static_cast<uint32_t>(rendered.size()), 0);
    ctx->gpr[3] = static_cast<uint32_t>(rendered.size());
}

extern "C" void No100f_HleSin(CpuContext* ctx) { ctx->fpr[1].d = std::sin(ctx->fpr[1].d); }
extern "C" void No100f_HleCos(CpuContext* ctx) { ctx->fpr[1].d = std::cos(ctx->fpr[1].d); }
extern "C" void No100f_HleTan(CpuContext* ctx) { ctx->fpr[1].d = std::tan(ctx->fpr[1].d); }
extern "C" void No100f_HleAtan(CpuContext* ctx) { ctx->fpr[1].d = std::atan(ctx->fpr[1].d); }
extern "C" void No100f_HleAtan2(CpuContext* ctx) { ctx->fpr[1].d = std::atan2(ctx->fpr[1].d, ctx->fpr[2].d); }
extern "C" void No100f_HleAsin(CpuContext* ctx) { ctx->fpr[1].d = std::asin(ctx->fpr[1].d); }
extern "C" void No100f_HleAcos(CpuContext* ctx) { ctx->fpr[1].d = std::acos(ctx->fpr[1].d); }
extern "C" void No100f_HleExp(CpuContext* ctx) { ctx->fpr[1].d = std::exp(ctx->fpr[1].d); }
extern "C" void No100f_HleSinh(CpuContext* ctx) { ctx->fpr[1].d = std::sinh(ctx->fpr[1].d); }
extern "C" void No100f_HleCosh(CpuContext* ctx) { ctx->fpr[1].d = std::cosh(ctx->fpr[1].d); }
extern "C" void No100f_HleFmod(CpuContext* ctx) { ctx->fpr[1].d = std::fmod(ctx->fpr[1].d, ctx->fpr[2].d); }
extern "C" void No100f_HlePow(CpuContext* ctx) { ctx->fpr[1].d = std::pow(ctx->fpr[1].d, ctx->fpr[2].d); }
extern "C" void No100f_HlePowf(CpuContext* ctx) {
    ctx->fpr[1].d = static_cast<double>(std::powf(static_cast<float>(ctx->fpr[1].d), static_cast<float>(ctx->fpr[2].d)));
}
extern "C" void No100f_HleFloor(CpuContext* ctx) { ctx->fpr[1].d = std::floor(ctx->fpr[1].d); }
extern "C" void No100f_HleSinf(CpuContext* ctx) {
    ctx->fpr[1].d = static_cast<double>(std::sinf(static_cast<float>(ctx->fpr[1].d)));
}
extern "C" void No100f_HleCosf(CpuContext* ctx) {
    ctx->fpr[1].d = static_cast<double>(std::cosf(static_cast<float>(ctx->fpr[1].d)));
}
extern "C" void No100f_HleTanf(CpuContext* ctx) {
    ctx->fpr[1].d = static_cast<double>(std::tanf(static_cast<float>(ctx->fpr[1].d)));
}

// Address identities in the 0x800F37EC-0x800F38F4 stub cluster were verified by
// decoding each wrapper's bl target in main.dol: MAP.txt's names are SHIFTED there
// (it labels the exp stub "cosh", the fmod stub "exp", the pow stub "sinh" and
// the powf stub "tanf"). The bindings below use the TRUE identities:
// 0x800F384C=exp, 0x800F386C=fmod, 0x800F388C=pow, 0x800F38AC=powf. Trusting MAP
// here hung guest init in build round 1 (the PR1 hang): with pow bound as sinh,
// __dec2num's 10^e scale came back sinh(5.0) for every exponent and the sd2.ini
// parse in __init_cpp never returned.
PPC_NATIVE_OVERRIDE_VOID(800EB118, No100f_HleSprintf, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F369C, No100f_HleSin, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F2FB0, No100f_HleCos, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F3774, No100f_HleTan, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F2D88, No100f_HleAtan, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F382C, No100f_HleAtan2, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F380C, No100f_HleAsin, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F37EC, No100f_HleAcos, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F386C, No100f_HleFmod, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F388C, No100f_HlePow, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F384C, No100f_HleExp, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F3084, No100f_HleFloor, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F38D0, No100f_HleSinf, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F38F4, No100f_HleCosf, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE_VOID(800F38AC, No100f_HlePowf, (CpuContext* ctx), (ctx));
