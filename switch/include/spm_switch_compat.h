// spm_switch_compat.h - force-included into every runtime and translated TU on the Switch build
// (switch/cmake/SpmSwitch.cmake). It fills the gaps between Horizon's newlib/libnx and the POSIX
// surface the runtime assumes, so the upstream sources compile without per-file edits.
#pragma once

#ifdef __SWITCH__

#include <setjmp.h>
#include <stdint.h>

// Horizon has no signals, so there is no signal mask for sigsetjmp to save. The runtime only uses
// the sig- pair so a POSIX SIGSEGV handler can longjmp out without leaving SIGSEGV blocked
// (system_bridge.h); with no signals, plain setjmp/longjmp is exactly equivalent.
#ifndef _SIGJMP_BUF_DEFINED
#define _SIGJMP_BUF_DEFINED
typedef jmp_buf sigjmp_buf;
#endif
#ifndef sigsetjmp
#define sigsetjmp(env, savemask) setjmp(env)
#endif
#ifndef siglongjmp
#define siglongjmp(env, val) longjmp(env, val)
#endif

// libnx's BSD socket headers omit the address-string length constants.
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

// Thread placement (switch/runtime/horizon_pthread.c): a thread calls fw_switch_thread_mark at its
// start to say what it is; the dk_diag.txt keys guestcore and bgprio decide what that changes (off:
// nothing but its boot log line). fw_switch_thread_log_current logs the calling thread's core,
// affinity and priority.
#define FW_THREAD_OTHER 0
#define FW_THREAD_RENDER 1      // Aurora's frame worker and presenter (deko3d replay and present)
#define FW_THREAD_BACKGROUND 2  // compile, cache writer, prefetch, watchdog, reaper
#ifdef __cplusplus
extern "C" {
#endif
void fw_switch_thread_mark(int kind);
void fw_switch_thread_log_current(const char* what, const void* fn);
// Priority 0x3B on every application core (exempt from guestcore): the debug server's threads.
void fw_switch_thread_lowest_priority(void);
#ifdef __cplusplus
}
#endif

#endif // __SWITCH__
