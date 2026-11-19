// NO100F 60 fps ([video] fps60): particle emitters keep the game's 30 Hz emission rate.
//
// The per-frame emitter update (0x800C4AAC) calls the emit function (0x800C4B04) with the frame time. Emitters with an
// emit interval of 0 (torches, candles) emit their count every call, so at 60 fps they made twice the particles. That
// fills the shared 2000-particle pool, and the allocator (0x80032728) then takes the newest particles of a
// lower-priority system: in "Mind your Manors" the floor fog lost two puffs a frame and refilled them at random spots,
// which flickered. Here an emitter that emits on every call (its timer +0x1C is 0) emits at most once per 30 Hz step:
// not in the frame right after it emitted, unless that frame was a full 30 Hz step (slowdown). It still gets this
// frame's time: emit sets a particle's velocity to the asset velocity times that time, and particles move by their
// velocity every frame. Emitters with a running timer count it down with the frame time, as before.
//
// A wrapper: REGISTER_NATIVE_FUNCTION_AS takes the address over but keeps the game's own update translated as
// func_800C4AAC, which this calls whenever the emitter may emit. With fps60 off that is every call, so the game runs
// its own code. The game's update does nothing while the game is paused (0x800AD3B4 returns nonzero) or the emitter is
// disabled (+0x18 bit 0); the gate asks the same first, so such a call never counts as an emission.
#include <cstdint>
#include <unordered_map>
#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_config.h"

extern "C" void func_800AD3B4(CpuContext* ctx);  // nonzero while the game is paused (reads a global; no side effects)
extern "C" void func_800C4AAC(CpuContext* ctx);  // the game's emitter update, kept translated

namespace {
constexpr uint32_t kFrameTime = 0x80270D84u;   // [r13 - 0x74FC]: measured frame time, clamped to 1/60..0.1 s
constexpr uint32_t kFrameCount = 0x80270890u;  // [r13 - 0x79F0]: main loop frame counter

// With fps60: false if this call would be an every-call emitter's second emission in one 30 Hz step.
bool EmitThisStep(CpuContext* ctx) {
    const uint32_t emitter = ctx->gpr[3];
    if (Memory::ReadFloat32(emitter + 0x1C) > 0.0f) return true;  // a running timer counts down with the frame time
    const double dt = ctx->fpr[1].d;
    func_800AD3B4(ctx);
    const bool idle = ctx->gpr[3] != 0 || (Memory::Read8(emitter + 0x18) & 1) == 0;
    ctx->gpr[3] = emitter;
    ctx->fpr[1].d = dt;
    if (idle) return true;  // the game's update returns without emitting
    static std::unordered_map<uint32_t, uint32_t> lastFrame;
    const uint32_t frame = Memory::Read32(kFrameCount);
    auto [it, fresh] = lastFrame.try_emplace(emitter, frame);
    if (!fresh && it->second != frame) {
        if (frame - it->second == 1 && Memory::ReadFloat32(kFrameTime) < 0.75f / 30.0f) return false;
        it->second = frame;
    }
    return true;
}
}  // namespace

extern "C" void No100f_UpdateParEmitter(CpuContext* ctx) {
    if (static const bool fps60 = RuntimeConfigFile::Fps60(); fps60 && !EmitThisStep(ctx)) return;
    func_800C4AAC(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x800C4AAC, No100f_UpdateParEmitter, "No100f_UpdateParEmitter");
