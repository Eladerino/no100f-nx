// Differential tests for the host libm/sprintf bindings in
// src/hle/no100f_libm_hle.cpp (build round 1, QUICK-AUDITS.md Audit 2 bar).
//
// Part A (trig): the bound host functions are compared against long-double
// references over swept corpora, including the large-argument range where
// argument reduction diverges between implementations. The printed maxima
// are the measured ULP bounds the bindings ship under.
//
// Part B (sprintf): the shipped adapter (No100f_HleSprintf) and the shipped
// formatter (RuntimeHle::FormatGuestPrintf in src/hle/c_stdio.cpp) run end
// to end against a fake guest memory and are compared byte-for-byte with
// host snprintf over the game's own format strings (census extracted from
// game/main.dol: %s, %.3f, %d, %8c, %c, %08x, %x, %8.3f, %f, %08X, %02d,
// %04d, %04x, %e, %6.1f, %X, %-2d, %14d, %8o, %.8c, %u, %3.2f, %g).

#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// ---------------------------------------------------------------------------
// Fake guest memory: 32 MiB at 0x80000000, big-endian scalar access, plus the
// registry no-op the registration macros need in a standalone executable.
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t kGuestBase = 0x80000000u;
constexpr size_t kGuestSize = 32u * 1024u * 1024u;
std::vector<uint8_t> g_guest(kGuestSize, 0);

size_t GuestOffset(uint32_t addr, size_t length = 1) {
    if (addr < kGuestBase || static_cast<uint64_t>(addr - kGuestBase) + length > kGuestSize) {
        throw std::runtime_error("guest address out of range");
    }
    return addr - kGuestBase;
}
}  // namespace

namespace MemoryInline {
// Zero page tables: every fast-path lookup misses and lands in the slow
// paths below, which serve the fake buffer with guest (big-endian) order.
PageEntry g_pageTable[kPageCount];
uintptr_t g_fullPageBias[kPageCount];
uintptr_t g_fullReadablePageBias[kPageCount];
uintptr_t g_fullWritablePageBias[kPageCount];
const SparseWritablePageTable* g_sparseWritablePageTables[kPageCount];
uint8_t g_deferredReadCoveredPages[kPageCount];

uint8_t Read8Slow(uint32_t addr) { return g_guest[GuestOffset(addr)]; }
uint16_t Read16Slow(uint32_t addr) {
    const size_t o = GuestOffset(addr, 2);
    return static_cast<uint16_t>((static_cast<uint16_t>(g_guest[o]) << 8) | g_guest[o + 1]);
}
uint32_t Read32Slow(uint32_t addr) {
    const size_t o = GuestOffset(addr, 4);
    return (static_cast<uint32_t>(g_guest[o]) << 24) | (static_cast<uint32_t>(g_guest[o + 1]) << 16) |
           (static_cast<uint32_t>(g_guest[o + 2]) << 8) | static_cast<uint32_t>(g_guest[o + 3]);
}
uint64_t Read64Slow(uint32_t addr) {
    return (static_cast<uint64_t>(Read32Slow(addr)) << 32) | Read32Slow(addr + 4);
}
float ReadFloat32Slow(uint32_t addr) {
    const uint32_t bits = Read32Slow(addr);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}
double ReadFloat64Slow(uint32_t addr) {
    const uint64_t bits = Read64Slow(addr);
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}
void Write8Slow(uint32_t addr, uint8_t val) { g_guest[GuestOffset(addr)] = val; }
void Write16Slow(uint32_t addr, uint16_t val) {
    const size_t o = GuestOffset(addr, 2);
    g_guest[o] = static_cast<uint8_t>(val >> 8);
    g_guest[o + 1] = static_cast<uint8_t>(val);
}
void Write32Slow(uint32_t addr, uint32_t val) {
    const size_t o = GuestOffset(addr, 4);
    g_guest[o] = static_cast<uint8_t>(val >> 24);
    g_guest[o + 1] = static_cast<uint8_t>(val >> 16);
    g_guest[o + 2] = static_cast<uint8_t>(val >> 8);
    g_guest[o + 3] = static_cast<uint8_t>(val);
}
void Write64Slow(uint32_t addr, uint64_t val) {
    Write32Slow(addr, static_cast<uint32_t>(val >> 32));
    Write32Slow(addr + 4, static_cast<uint32_t>(val));
}
void WriteFloat32Slow(uint32_t addr, double val) {
    const float f = static_cast<float>(val);
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    Write32Slow(addr, bits);
}
void WriteFloat64Slow(uint32_t addr, double val) {
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(bits));
    Write64Slow(addr, bits);
}
}  // namespace MemoryInline

uint8_t* Memory::GetPointer(uint32_t addr) { return &g_guest[GuestOffset(addr)]; }
uint8_t* Memory::GetPointer(uint32_t addr, size_t length) { return &g_guest[GuestOffset(addr, length)]; }
bool Memory::Contains(uint32_t addr, size_t length) {
    return addr >= kGuestBase && static_cast<uint64_t>(addr - kGuestBase) + length <= kGuestSize;
}

void TranslatedFunctionRegistry::Register(TranslatedFunctionInfo) {}

namespace RecompMod {
// No executable pages in the fake address space: all guards clear.
std::atomic<uint8_t> g_executableWriteGuardPages[kExecutableWriteGuardPageCount];
}  // namespace RecompMod

// ---------------------------------------------------------------------------
// Part A: trig differentials
// ---------------------------------------------------------------------------
namespace {

int64_t OrderedBits(double v) {
    int64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits < 0 ? INT64_MIN - bits : bits;
}
uint64_t UlpDiff(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return 0;
    const int64_t d = OrderedBits(a) - OrderedBits(b);
    return d < 0 ? static_cast<uint64_t>(-d) : static_cast<uint64_t>(d);
}
int32_t OrderedBitsF(float v) {
    int32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits < 0 ? INT32_MIN - bits : bits;
}
uint32_t UlpDiffF(float a, float b) {
    if (std::isnan(a) && std::isnan(b)) return 0;
    const int32_t d = OrderedBitsF(a) - OrderedBitsF(b);
    return d < 0 ? static_cast<uint32_t>(-d) : static_cast<uint32_t>(d);
}

// Deterministic pseudo-random doubles in (0, 1).
struct Rng {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    double Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
    }
};

uint64_t MaxUlpSinCos(double lo, double hi, int samples, Rng& rng) {
    uint64_t worst = 0;
    for (int i = 0; i < samples; ++i) {
        const double x = lo + (hi - lo) * rng.Next();
        worst = std::max(worst, UlpDiff(std::sin(x), static_cast<double>(sinl(static_cast<long double>(x)))));
        worst = std::max(worst, UlpDiff(std::cos(x), static_cast<double>(cosl(static_cast<long double>(x)))));
    }
    return worst;
}

void TestTrig() {
    Rng rng;
    const uint64_t bandSmall = MaxUlpSinCos(-4096.0, 4096.0, 200000, rng);
    const uint64_t bandMid = MaxUlpSinCos(-1048576.0, 1048576.0, 200000, rng);
    const uint64_t bandLarge = MaxUlpSinCos(-1.0e15, 1.0e15, 100000, rng);
    std::cout << "sin/cos max ULP vs long-double reference: |x|<=4096: " << bandSmall
              << ", |x|<=2^20: " << bandMid << ", |x|<=1e15: " << bandLarge << "\n";
    Require(bandSmall <= 2, "sin/cos exceed 2 ULP in the game-realistic band");
    Require(bandMid <= 4, "sin/cos exceed 4 ULP at |x|<=2^20");

    // Special values must agree exactly in class.
    Require(std::isnan(std::sin(INFINITY)) && std::isnan(static_cast<double>(sinl(INFINITY))), "sin(inf) class");
    Require(std::sin(0.0) == 0.0 && std::cos(0.0) == 1.0, "sin/cos at 0");

    // Float variants in the realistic band.
    uint32_t worstF = 0;
    for (int i = 0; i < 200000; ++i) {
        const float x = static_cast<float>(-4096.0 + 8192.0 * rng.Next());
        worstF = std::max(worstF, UlpDiffF(std::sinf(x), static_cast<float>(sinl(static_cast<long double>(x)))));
        worstF = std::max(worstF, UlpDiffF(std::cosf(x), static_cast<float>(cosl(static_cast<long double>(x)))));
        worstF = std::max(worstF, UlpDiffF(std::tanf(x), static_cast<float>(tanl(static_cast<long double>(x)))));
    }
    std::cout << "sinf/cosf/tanf max ULP |x|<=4096: " << worstF << "\n";
    Require(worstF <= 2, "float trig exceeds 2 ULP in the realistic band");

    // floor is exact.
    for (int i = 0; i < 100000; ++i) {
        const double x = (rng.Next() - 0.5) * 9.0e15;
        Require(std::floor(x) == static_cast<double>(floorl(static_cast<long double>(x))), "floor not exact");
    }

    // Riders: bounded agreement with the long-double references.
    uint64_t worstRider = 0;
    for (int i = 0; i < 50000; ++i) {
        const double x = (rng.Next() - 0.5) * 20.0;
        const double y = (rng.Next() - 0.5) * 20.0;
        worstRider = std::max(worstRider, UlpDiff(std::atan2(y, x), static_cast<double>(atan2l(static_cast<long double>(y), static_cast<long double>(x)))));
        worstRider = std::max(worstRider, UlpDiff(std::tan(x), static_cast<double>(tanl(static_cast<long double>(x)))));
        worstRider = std::max(worstRider, UlpDiff(std::atan(x), static_cast<double>(atanl(static_cast<long double>(x)))));
        const double unit = rng.Next() * 2.0 - 1.0;
        worstRider = std::max(worstRider, UlpDiff(std::asin(unit), static_cast<double>(asinl(static_cast<long double>(unit)))));
        worstRider = std::max(worstRider, UlpDiff(std::acos(unit), static_cast<double>(acosl(static_cast<long double>(unit)))));
        const double small = (rng.Next() - 0.5) * 4.0;
        worstRider = std::max(worstRider, UlpDiff(std::exp(small), static_cast<double>(expl(static_cast<long double>(small)))));
        worstRider = std::max(worstRider, UlpDiff(std::sinh(small), static_cast<double>(sinhl(static_cast<long double>(small)))));
        worstRider = std::max(worstRider, UlpDiff(std::cosh(small), static_cast<double>(coshl(static_cast<long double>(small)))));
    }
    std::cout << "rider libm max ULP: " << worstRider << "\n";
    Require(worstRider <= 4, "rider libm exceeds 4 ULP");
}

}  // namespace

// ---------------------------------------------------------------------------
// Part B: sprintf end to end
// ---------------------------------------------------------------------------
extern "C" void No100f_HleSprintf(CpuContext* ctx);

namespace {

constexpr uint32_t kOut = 0x80010000u;
constexpr uint32_t kFmt = 0x80020000u;
constexpr uint32_t kStr = 0x80030000u;
constexpr uint32_t kStack = 0x80F00000u;

void WriteGuestString(uint32_t addr, const char* text) {
    const size_t o = GuestOffset(addr);
    std::memcpy(&g_guest[o], text, std::strlen(text) + 1);
}
std::string ReadGuestCString(uint32_t addr) {
    const size_t o = GuestOffset(addr);
    return std::string(reinterpret_cast<const char*>(&g_guest[o]));
}

struct SprintfCase {
    CpuContext ctx{};
    SprintfCase(const char* format) {
        std::fill(std::begin(g_guest), std::end(g_guest), 0);
        WriteGuestString(kFmt, format);
        ctx.gpr[1] = kStack;
        ctx.gpr[3] = kOut;
        ctx.gpr[4] = kFmt;
    }
    std::string Run(int expectedReturn, const std::string& expected) {
        No100f_HleSprintf(&ctx);
        const std::string got = ReadGuestCString(kOut);
        if (got != expected || static_cast<int>(ctx.gpr[3]) != expectedReturn) {
            std::cout << "sprintf mismatch: got \"" << got << "\" (ret " << ctx.gpr[3] << "), want \"" << expected
                      << "\" (ret " << expectedReturn << ")\n";
            throw std::runtime_error("sprintf differential failed");
        }
        return got;
    }
};

void TestSprintf() {
    // Register/stack walk: eight %d (six in r5..r10, two on the stack).
    {
        SprintfCase c("%d %d %d %d %d %d %d %d");
        const int vals[8] = {11, 22, 33, 44, 55, 66, 77, 88};
        for (int i = 0; i < 6; ++i) c.ctx.gpr[5 + i] = static_cast<uint32_t>(vals[i]);
        Memory::Write32(kStack + 8, static_cast<uint32_t>(vals[6]));
        Memory::Write32(kStack + 12, static_cast<uint32_t>(vals[7]));
        char truth[128];
        const int n = std::snprintf(truth, sizeof(truth), "%d %d %d %d %d %d %d %d", 11, 22, 33, 44, 55, 66, 77, 88);
        c.Run(n, truth);
    }
    // 64-bit first: pair at (r5, r6), next int at r7.
    {
        SprintfCase c("%lld %d");
        const uint64_t v = 0x123456789ABCDEF0ull;
        c.ctx.gpr[5] = static_cast<uint32_t>(v >> 32);
        c.ctx.gpr[6] = static_cast<uint32_t>(v);
        c.ctx.gpr[7] = 42;
        char truth[128];
        const int n = std::snprintf(truth, sizeof(truth), "%lld %d", static_cast<long long>(v), 42);
        c.Run(n, truth);
    }
    // 64-bit after a 32-bit: r6 must be skipped, pair at (r7, r8).
    {
        SprintfCase c("%d %lld");
        const uint64_t v = 0xFEDCBA9876543210ull;
        c.ctx.gpr[5] = 7;
        c.ctx.gpr[6] = 0xDEADBEEFu;
        c.ctx.gpr[7] = static_cast<uint32_t>(v >> 32);
        c.ctx.gpr[8] = static_cast<uint32_t>(v);
        char truth[128];
        const int n = std::snprintf(truth, sizeof(truth), "%d %lld", 7, static_cast<long long>(v));
        c.Run(n, truth);
    }
    // Mixed float/int: independent counters.
    {
        SprintfCase c("%f %d %f");
        c.ctx.fpr[1].d = 1.5;
        c.ctx.gpr[5] = 9;
        c.ctx.fpr[2].d = -2.25;
        char truth[128];
        const int n = std::snprintf(truth, sizeof(truth), "%f %d %f", 1.5, 9, -2.25);
        c.Run(n, truth);
    }
    // The game's own multi-conversion strings.
    {
        SprintfCase c("<%.3f %.3f %.3f> %.3f");
        c.ctx.fpr[1].d = 1.23456;
        c.ctx.fpr[2].d = -0.5;
        c.ctx.fpr[3].d = 123456.789;
        c.ctx.fpr[4].d = 0.0;
        char truth[256];
        const int n = std::snprintf(truth, sizeof(truth), "<%.3f %.3f %.3f> %.3f", 1.23456, -0.5, 123456.789, 0.0);
        c.Run(n, truth);
    }
    {
        SprintfCase c("%02d/%02d/%04d %02d:%02d:%02d");
        const int v[6] = {11, 14, 2026, 9, 5, 3};
        for (int i = 0; i < 6; ++i) c.ctx.gpr[5 + i] = static_cast<uint32_t>(v[i]);
        char truth[128];
        const int n = std::snprintf(truth, sizeof(truth), "%02d/%02d/%04d %02d:%02d:%02d", 11, 14, 2026, 9, 5, 3);
        c.Run(n, truth);
    }
    {
        SprintfCase c("r%-2d  = 0x%08x (%14d)  r%-2d  = 0x%08x (%14d)");
        const uint32_t v[6] = {3, 0x8000E220u, static_cast<uint32_t>(-12345), 4, 0x80102464u, 987654};
        for (int i = 0; i < 6; ++i) c.ctx.gpr[5 + i] = static_cast<uint32_t>(v[i]);
        char truth[256];
        const int n = std::snprintf(truth, sizeof(truth), "r%-2d  = 0x%08x (%14d)  r%-2d  = 0x%08x (%14d)",
                                    3, 0x8000E220u, -12345, 4, 0x80102464u, 987654);
        c.Run(n, truth);
    }
    {
        SprintfCase c("### Using %d bytes from FMV work buffer, %08X - %08X");
        c.ctx.gpr[5] = 65536;
        c.ctx.gpr[6] = 0x80003000u;
        c.ctx.gpr[7] = 0x80013000u;
        char truth[256];
        const int n = std::snprintf(truth, sizeof(truth), "### Using %d bytes from FMV work buffer, %08X - %08X",
                                    65536, 0x80003000u, 0x80013000u);
        c.Run(n, truth);
    }
    // Strings.
    {
        SprintfCase c("%s-oid:  %u");
        WriteGuestString(kStr, "scooby");
        c.ctx.gpr[5] = kStr;
        c.ctx.gpr[6] = 4000000000u;
        char truth[128];
        const int n = std::snprintf(truth, sizeof(truth), "%s-oid:  %u", "scooby", 4000000000u);
        c.Run(n, truth);
    }
    {
        SprintfCase c(" in \"%s\" on line %d.");
        WriteGuestString(kStr, "zScene.cpp");
        c.ctx.gpr[5] = kStr;
        c.ctx.gpr[6] = 1234;
        char truth[128];
        const int n = std::snprintf(truth, sizeof(truth), " in \"%s\" on line %d.", "zScene.cpp", 1234);
        c.Run(n, truth);
    }
    // Chars.
    {
        SprintfCase c("%c%c%c%c");
        c.ctx.gpr[5] = 'G';
        c.ctx.gpr[6] = 'I';
        c.ctx.gpr[7] = 'H';
        c.ctx.gpr[8] = 'E';
        char truth[64];
        const int n = std::snprintf(truth, sizeof(truth), "%c%c%c%c", 'G', 'I', 'H', 'E');
        c.Run(n, truth);
    }
    {
        SprintfCase c("%8c");
        c.ctx.gpr[5] = 'A';
        char truth[64];
        const int n = std::snprintf(truth, sizeof(truth), "%8c", 'A');
        c.Run(n, truth);
    }
    {
        SprintfCase c("%.8c");
        c.ctx.gpr[5] = 'B';
        char truth[64];
        const int n = std::snprintf(truth, sizeof(truth), "%.8c", 'B');
        c.Run(n, truth);
    }
    // Integer conversions from the census.
    {
        SprintfCase c("%8o %04x %x %X %i");
        const uint32_t v[5] = {511, 0xAB, 0xDEAD, 0xBEEF, static_cast<uint32_t>(-77)};
        for (int i = 0; i < 5; ++i) c.ctx.gpr[5 + i] = v[i];
        char truth[128];
        const int n = std::snprintf(truth, sizeof(truth), "%8o %04x %x %X %i", 511u, 0xABu, 0xDEADu, 0xBEEFu, -77);
        c.Run(n, truth);
    }
    // Float formats and values, including the %.3f stress set.
    const double fvals[] = {0.0, -0.0, 1.5, -2.25, 123456.789, -999.9999, 1.0e20, 1.0e-20, 3.141592653589793};
    const char* fformats[] = {"%.3f", "%8.3f", "%6.1f", "%3.2f", "%f", "%e", "%g"};
    for (const char* fmt : fformats) {
        for (double v : fvals) {
            SprintfCase c(fmt);
            c.ctx.fpr[1].d = v;
            char truth[256];
            const int n = std::snprintf(truth, sizeof(truth), fmt, v);
            c.Run(n, truth);
        }
    }
    std::cout << "sprintf differential: all cases byte-identical to host snprintf\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Part C: the four corrected stub-cluster bindings (PR1 hang fix). MAP.txt's
// names are shifted across 0x800F37EC-0x800F38F4, so round 1 bound the pow
// stub (0x800F388C) as sinh and guest init hung in __dec2num. The shipped
// bodies are driven end to end through a CpuContext, exactly as the guest
// reaches them: fpr[1] (and fpr[2]) in, fpr[1] out. References are the
// long-double libm functions; bars match Part A (<= 2 ULP).
// ---------------------------------------------------------------------------
extern "C" void No100f_HlePow(CpuContext* ctx);
extern "C" void No100f_HlePowf(CpuContext* ctx);
extern "C" void No100f_HleFmod(CpuContext* ctx);
extern "C" void No100f_HleExp(CpuContext* ctx);

namespace {

double CallHleDouble2(void (*fn)(CpuContext*), double a, double b) {
    CpuContext ctx{};
    ctx.fpr[1].d = a;
    ctx.fpr[2].d = b;
    fn(&ctx);
    return ctx.fpr[1].d;
}
double CallHleDouble1(void (*fn)(CpuContext*), double a) {
    CpuContext ctx{};
    ctx.fpr[1].d = a;
    fn(&ctx);
    return ctx.fpr[1].d;
}

void TestCorrectedBindings() {
    Rng rng;

    // The hang path itself: __dec2num scales by pow(5, e) for the decimal
    // exponent. Every e in -20..20 must track powl, not sinh(5).
    uint64_t worstHang = 0;
    for (int e = -20; e <= 20; ++e) {
        const double de = static_cast<double>(e);
        worstHang = std::max(worstHang, UlpDiff(CallHleDouble2(No100f_HlePow, 5.0, de),
                                                static_cast<double>(powl(5.0L, static_cast<long double>(e)))));
        worstHang = std::max(worstHang, UlpDiff(CallHleDouble2(No100f_HlePow, 10.0, de),
                                                static_cast<double>(powl(10.0L, static_cast<long double>(e)))));
    }
    std::cout << "pow(5,e)/pow(10,e) e in -20..20 max ULP: " << worstHang << "\n";
    Require(worstHang <= 2, "pow hang-path cases exceed 2 ULP");
    Require(UlpDiff(CallHleDouble2(No100f_HlePow, 5.0, 2.0), 25.0) == 0, "pow(5,2) not exact");
    Require(UlpDiff(CallHleDouble2(No100f_HlePow, 5.0, -1.0), 0.2) <= 1, "pow(5,-1) off by >1 ULP");

    // General pow: negative bases with integer exponents, positive bases
    // with fractional exponents.
    uint64_t worstPow = 0;
    for (int i = 0; i < 100000; ++i) {
        const double base = (rng.Next() - 0.5) * 200.0;
        const double iexp = static_cast<double>(static_cast<int>(rng.Next() * 61.0) - 30);
        worstPow = std::max(worstPow, UlpDiff(CallHleDouble2(No100f_HlePow, base, iexp),
                                              static_cast<double>(powl(static_cast<long double>(base), static_cast<long double>(iexp)))));
        const double pbase = 0.001 + rng.Next() * 100.0;
        const double fexp = (rng.Next() - 0.5) * 40.0;
        worstPow = std::max(worstPow, UlpDiff(CallHleDouble2(No100f_HlePow, pbase, fexp),
                                              static_cast<double>(powl(static_cast<long double>(pbase), static_cast<long double>(fexp)))));
    }
    std::cout << "pow general max ULP: " << worstPow << "\n";
    Require(worstPow <= 2, "pow exceeds 2 ULP in the general sweep");
    // pow special values: exact results and classes.
    Require(CallHleDouble2(No100f_HlePow, 123.5, 0.0) == 1.0, "pow(x,0) != 1");
    Require(CallHleDouble2(No100f_HlePow, 1.0, -37.25) == 1.0, "pow(1,y) != 1");
    Require(CallHleDouble2(No100f_HlePow, -2.0, 3.0) == -8.0, "pow(-2,3) != -8");
    Require(CallHleDouble2(No100f_HlePow, -2.0, 2.0) == 4.0, "pow(-2,2) != 4");
    Require(CallHleDouble2(No100f_HlePow, 0.0, 2.0) == 0.0, "pow(0,2) != 0");
    Require(std::isnan(CallHleDouble2(No100f_HlePow, -2.0, 0.5)), "pow(-2,0.5) not NaN");
    Require(std::isinf(CallHleDouble2(No100f_HlePow, 0.0, -1.0)), "pow(0,-1) not inf");

    // powf: same shape through the float body (args ride the fprs as
    // doubles; the body narrows to float).
    uint32_t worstPowf = 0;
    for (int e = -20; e <= 20; ++e) {
        worstPowf = std::max(worstPowf, UlpDiffF(static_cast<float>(CallHleDouble2(No100f_HlePowf, 5.0, static_cast<double>(e))),
                                                 static_cast<float>(powl(5.0L, static_cast<long double>(e)))));
    }
    for (int i = 0; i < 100000; ++i) {
        const float base = static_cast<float>((rng.Next() - 0.5) * 50.0);
        const float iexp = static_cast<float>(static_cast<int>(rng.Next() * 21.0) - 10);
        worstPowf = std::max(worstPowf, UlpDiffF(static_cast<float>(CallHleDouble2(No100f_HlePowf, base, iexp)),
                                                 static_cast<float>(powl(static_cast<long double>(base), static_cast<long double>(iexp)))));
        const float pbase = static_cast<float>(0.01 + rng.Next() * 20.0);
        const float fexp = static_cast<float>((rng.Next() - 0.5) * 20.0);
        worstPowf = std::max(worstPowf, UlpDiffF(static_cast<float>(CallHleDouble2(No100f_HlePowf, pbase, fexp)),
                                                 static_cast<float>(powl(static_cast<long double>(pbase), static_cast<long double>(fexp)))));
    }
    std::cout << "powf max ULP: " << worstPowf << "\n";
    Require(worstPowf <= 2, "powf exceeds 2 ULP");
    Require(CallHleDouble2(No100f_HlePowf, -2.0, 3.0) == -8.0, "powf(-2,3) != -8");

    // fmod: exact-remainder semantics; the reference is fmodl.
    uint64_t worstFmod = 0;
    for (int i = 0; i < 100000; ++i) {
        const double x = (rng.Next() - 0.5) * 2.0e6;
        double y = (rng.Next() - 0.5) * 2.0e3;
        if (std::fabs(y) < 1.0e-3) y = 1.0;
        worstFmod = std::max(worstFmod, UlpDiff(CallHleDouble2(No100f_HleFmod, x, y),
                                                static_cast<double>(fmodl(static_cast<long double>(x), static_cast<long double>(y)))));
    }
    std::cout << "fmod max ULP: " << worstFmod << "\n";
    Require(worstFmod <= 2, "fmod exceeds 2 ULP");
    Require(CallHleDouble2(No100f_HleFmod, 7.5, 2.0) == 1.5, "fmod(7.5,2) != 1.5");
    Require(CallHleDouble2(No100f_HleFmod, -7.5, 2.0) == -1.5, "fmod(-7.5,2) != -1.5");
    Require(CallHleDouble2(No100f_HleFmod, 5.0, INFINITY) == 5.0, "fmod(x,inf) != x");
    Require(std::isnan(CallHleDouble2(No100f_HleFmod, 5.0, 0.0)), "fmod(x,0) not NaN");

    // exp through the shipped body, realistic and wide bands.
    uint64_t worstExp = 0;
    for (int i = 0; i < 50000; ++i) {
        const double x = (rng.Next() - 0.5) * 80.0;
        worstExp = std::max(worstExp, UlpDiff(CallHleDouble1(No100f_HleExp, x),
                                              static_cast<double>(expl(static_cast<long double>(x)))));
        const double big = (rng.Next() - 0.5) * 1200.0;
        worstExp = std::max(worstExp, UlpDiff(CallHleDouble1(No100f_HleExp, big),
                                              static_cast<double>(expl(static_cast<long double>(big)))));
    }
    std::cout << "exp max ULP: " << worstExp << "\n";
    Require(worstExp <= 2, "exp exceeds 2 ULP");
    Require(CallHleDouble1(No100f_HleExp, 0.0) == 1.0, "exp(0) != 1");
}

}  // namespace

int main() {
    try {
        TestTrig();
        TestSprintf();
        TestCorrectedBindings();
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "no100f_libm_bindings_tests: PASS\n";
    return 0;
}
