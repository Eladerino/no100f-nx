// The FrightWorks commands for the shared debug server core (debug_server.h, vendored from
// SwitchWakerHD through spm-nx: the core knows sockets, lines and files; this file is the part
// that knows FrightWorks). Shaped after spm-nx's switch/source/spm_debug_server.cpp (12ec5e7).
//
// The gate is the build: FW_SWITCH_DEBUG_SERVER (switch/cmake/FrightWorksSwitch.cmake, on for dev
// builds) makes userAppInit (fw_switch_init.cpp) call fw_debug_server_boot once the sockets are
// up. Off, nothing here runs: no socket, no thread, no cost. A settings row comes later; H2 needs
// the server from the first boot, before any menu exists.
//
// What the server adds over the core's built-ins (ping, help, get, put, ls, rm, mkdir, log,
// logtext, press, hold, release, stick):
//   info, uptime, flush, mark: enough for a scripted null-backend run to check the game is alive,
//     bracket a test in the log and pull the log without a reboot;
//   shot, warp, peek, poke, quit, reload answer err and say why: H2 has no renderer to capture,
//     the level warp needs the dev menu's hook, guest reads need a mapping-checked read, and
//     quit/reload need the hbloader next-load path. They are follow-ups.
// Input injection needs no command here: the core holds the injected state and the provider at the
// bottom of this file feeds it to the SDL gamepad shim's weak cos_switch_debug_input hook.
//
// The log feed: the runtime's log writer is stderr (redirected to UserData/Logs/stderr.log,
// line-buffered). A pump thread tails that file from its start into log_tap, so the stream carries
// the whole session, boot lines included, without touching the game's log call sites. The pump and
// the core's threads run at the lowest thread priority (0x3B): a scripted run must never take time
// from the game it is measuring.

#include "debug_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

extern "C" void fw_switch_keep_awake(bool on);

namespace FwDebugServer {
namespace {

std::atomic<bool> g_booted{false};
std::atomic<bool> g_pumpRun{false};
std::thread g_pump;
std::string g_root;  // the server's file root (Config.root), set at boot
const auto g_bootTime = std::chrono::steady_clock::now();

void LogLine(const char* line) {
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
}

std::string Joined(const debugsrv::Args& a) {
    std::string text;
    for (size_t i = 0; i < a.size(); i++) {
        if (i) text += ' ';
        text += a[i];
    }
    return text;
}

long long UptimeSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_bootTime)
        .count();
}

debugsrv::Reply CmdInfo(const debugsrv::Args&) {
    char text[320];
    std::snprintf(text, sizeof text,
                  "game FrightWorks (GIHE78)\nbackend null (H2: no renderer)\nuptime %lld s\n"
                  "server port 6543\nroot %s\n",
                  UptimeSeconds(), g_root.c_str());
    return debugsrv::ok(text);
}

debugsrv::Reply CmdUptime(const debugsrv::Args&) {
    return debugsrv::ok(std::to_string(UptimeSeconds()) + "\n");
}

debugsrv::Reply CmdFlush(const debugsrv::Args&) {
    std::fflush(stderr);
    return debugsrv::ok();
}

debugsrv::Reply CmdMark(const debugsrv::Args& a) {
    std::fprintf(stderr, "[server] mark \"%s\" at %lld s\n", Joined(a).c_str(), UptimeSeconds());
    std::fflush(stderr);
    return debugsrv::ok();
}

// ---- not in this build (the follow-ups, answered honestly) ------------------------------

debugsrv::Reply CmdShot(const debugsrv::Args&) {
    return debugsrv::err("shot is not in this build: H2 runs on Dawn's Null backend, so there is no frame to "
                         "capture. It arrives with the deko3d present (H3)");
}

debugsrv::Reply CmdWarp(const debugsrv::Args&) {
    return debugsrv::err("warp is not in this build: it needs a hook into the dev menu's level load. It is a "
                         "follow-up");
}

debugsrv::Reply CmdPeek(const debugsrv::Args&) {
    return debugsrv::err("peek is not in this build: the flat guest window faults on an unmapped read, so "
                         "peeking first needs a mapping-checked read. It is a follow-up");
}

debugsrv::Reply CmdPoke(const debugsrv::Args&) {
    return debugsrv::err("poke is not in this build (see peek)");
}

debugsrv::Reply CmdQuit(const debugsrv::Args&) {
    return debugsrv::err("quit is not in this build: nothing yet ends the program on the server's word. It is a "
                         "follow-up, with reload, through the hbloader next-load path");
}

debugsrv::Reply CmdReload(const debugsrv::Args&) {
    return debugsrv::err("reload is not in this build: it needs the hbloader next-load path. It is a follow-up");
}

// ---- the log pump ------------------------------------------------------------------------

void PumpMain() {
#ifdef __SWITCH__
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
#endif
    const std::string path = g_root + "/UserData/Logs/stderr.log";
    std::vector<char> buf(64 << 10);
    long offset = 0;
    FILE* f = nullptr;
    while (g_pumpRun.load()) {
        if (!f) {
            f = std::fopen(path.c_str(), "r");
            offset = 0;
        }
        if (f) {
            std::fflush(stderr);
            if (std::fseek(f, 0, SEEK_END) == 0) {
                const long size = std::ftell(f);
                if (size < offset) offset = 0;  // the file was truncated: a new boot, start over
                if (size > offset && std::fseek(f, offset, SEEK_SET) == 0) {
                    long left = size - offset;
                    while (left > 0 && g_pumpRun.load()) {
                        const size_t want = size_t(std::min<long>(left, (long)buf.size()));
                        const size_t got = std::fread(buf.data(), 1, want, f);
                        if (!got) break;
                        debugsrv::log_tap(buf.data(), got);
                        offset += (long)got;
                        left -= (long)got;
                    }
                }
            }
        }
        // 100 ms between passes, in slices so stop() joins at once
        for (int i = 0; i < 10 && g_pumpRun.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (f) std::fclose(f);
}

void ThreadStart() {
#ifdef __SWITCH__
    // The lowest priority: the server's threads only run in the game's gaps.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
#endif
}

}  // namespace
}  // namespace FwDebugServer

extern "C" void fw_debug_server_boot(const char* root) {
    using namespace FwDebugServer;
    if (g_booted.exchange(true)) return;
    g_root = root ? root : "";
    // The pump replays the log file from its start, so the stream covers the boot too;
    // keep_log covers the window between here and start().
    debugsrv::keep_log();
    debugsrv::add_command("info", "info                   game, backend, uptime, root", &CmdInfo);
    debugsrv::add_command("uptime", "uptime                 seconds since the server booted", &CmdUptime);
    debugsrv::add_command("flush", "flush                  flush stderr.log now", &CmdFlush);
    debugsrv::add_command("mark", "mark <text>            a [server] mark line in the log, then a flush", &CmdMark);
    debugsrv::add_command("shot", "shot                   not in this build (null backend)", &CmdShot);
    debugsrv::add_command("warp", "warp <level>           not in this build", &CmdWarp);
    debugsrv::add_command("peek", "peek <addr> [bytes]    not in this build", &CmdPeek);
    debugsrv::add_command("poke", "poke <addr> <word>     not in this build", &CmdPoke);
    debugsrv::add_command("quit", "quit                   not in this build", &CmdQuit);
    debugsrv::add_command("reload", "reload                 not in this build", &CmdReload);

    debugsrv::Config config;
    config.port = 6543;
    config.root = g_root;
    config.log = &LogLine;
    config.threadStart = &ThreadStart;
    if (!debugsrv::start(config)) {
        debugsrv::drop_log();
        return;
    }
    LogLine("[server] debug server listening on port 6543: dev builds only, local network, NO password "
            "(anyone who can reach the port can move files under the root and press buttons)");
    fw_switch_keep_awake(true);
    g_pumpRun = true;
    g_pump = std::thread(&PumpMain);
}

extern "C" void fw_debug_server_stop(void) {
    using namespace FwDebugServer;
    if (!g_booted.load()) return;
    g_pumpRun = false;
    if (g_pump.joinable()) g_pump.join();
    debugsrv::stop();
    fw_switch_keep_awake(false);
}

#ifdef __SWITCH__
// The SDL gamepad shim's input poll (switch/sdl3_shim/sdl3_shim_gamepad.c) calls this through a
// weak declaration: the injected presses and sticks as if from the controller, and while the server
// runs the controller counts as connected even with none attached. Keep it in this translation
// unit: fw_debug_server_boot is what pulls the unit into the link, so the strong symbol is there for
// the shim's weak call whenever the server code is.
extern "C" int cos_switch_debug_input(uint64_t* buttons, int32_t sticks[4]) {
    if (!debugsrv::running()) return 0;
    const debugsrv::Injected in = debugsrv::injected();
    *buttons |= in.buttons;
    for (int s = 0; s < 2; s++) {
        if (in.stick[s]) {
            sticks[2 * s] = int32_t(in.x[s] * JOYSTICK_MAX);
            sticks[2 * s + 1] = int32_t(in.y[s] * JOYSTICK_MAX);
        }
    }
    return 1;
}
#endif
