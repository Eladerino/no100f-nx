// SDL 3 calls the FrightWorks runtime makes that spm-nx's runtime does not (H1a symbol audit).
// The Switch has one gamepad slot (handheld or player 1, sdl3_shim_gamepad.c), no browser and no
// windowing system, so these answer the way SDL does on a platform without the feature.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>

#include "sdl3_shim.h"

#define INSTANCE_ID 1

bool SDL_GamepadConnected(SDL_Gamepad* gamepad) {
    return gamepad != NULL && gamepad == sdl3_shim_gamepad_if_open() && sdl3_shim_gamepad_connected();
}

SDL_GamepadType SDL_GetGamepadTypeForID(SDL_JoystickID instance_id) {
    return instance_id == INSTANCE_ID ? SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO : SDL_GAMEPAD_TYPE_UNKNOWN;
}

bool SDL_OpenURL(const char* url) {
    (void)url;
    sdl3_shim_set_error("SDL_OpenURL is not supported on Horizon");
    return false;
}

// No windowing system: log the message so it lands in the NRO's stderr log, then report success
// so callers continue as they do after the user dismisses the box.
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char* title, const char* message,
                              SDL_Window* window) {
    (void)flags;
    (void)window;
    fprintf(stderr, "[switch] message box: %s: %s\n", title ? title : "", message ? message : "");
    return true;
}

// Called by FrightWorks' Aurora (window.cpp, window_owner.cpp, aurora.cpp) on the link path.
// One fixed full-screen window at the origin; no mouse, no keyboard modifiers, no cursor.
bool SDL_SetAppMetadataProperty(const char* name, const char* value) {
    (void)name;
    (void)value;
    return true;
}
SDL_WindowID SDL_GetWindowID(SDL_Window* window) { return window != NULL ? 1 : 0; }
bool SDL_GetWindowPosition(SDL_Window* window, int* x, int* y) {
    (void)window;
    if (x != NULL)
        *x = 0;
    if (y != NULL)
        *y = 0;
    return true;
}
bool SDL_GetWindowRelativeMouseMode(SDL_Window* window) {
    (void)window;
    return false;
}
SDL_Keymod SDL_GetModState(void) { return SDL_KMOD_NONE; }
SDL_Cursor* SDL_GetCursor(void) { return NULL; }

// ---- Display, thread and event calls FrightWorks' window owner makes (aurora-main/lib/window_owner.cpp,
// window.cpp). The window owner only runs on Windows (runtime/src/main.cpp), so on the Switch these are
// for the link: one display, the panel's size, the main thread is libnx's, SDL is always initialised.

SDL_DisplayID* SDL_GetDisplays(int* count) {
    SDL_DisplayID* ids = (SDL_DisplayID*)malloc(2 * sizeof(SDL_DisplayID));
    if (ids == NULL) {
        if (count != NULL)
            *count = 0;
        return NULL;
    }
    ids[0] = 1;
    ids[1] = 0;
    if (count != NULL)
        *count = 1;
    return ids;
}
const char* SDL_GetDisplayName(SDL_DisplayID displayID) { return displayID == 1 ? "Nintendo Switch" : NULL; }
bool SDL_GetDisplayBounds(SDL_DisplayID displayID, SDL_Rect* rect) {
    if (displayID != 1 || rect == NULL)
        return SDL_SetError("SDL_GetDisplayBounds: no such display");
    rect->x = 0;
    rect->y = 0;
    rect->w = SDL3_SHIM_WIDTH;
    rect->h = SDL3_SHIM_HEIGHT;
    return true;
}
bool SDL_GetDisplayUsableBounds(SDL_DisplayID displayID, SDL_Rect* rect) {
    return SDL_GetDisplayBounds(displayID, rect);
}
float SDL_GetDisplayContentScale(SDL_DisplayID displayID) { return displayID == 1 ? 1.0f : 0.0f; }

bool SDL_IsMainThread(void) { return threadGetCurHandle() == envGetMainThreadHandle(); }
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) { return flags; }

// Only SDL_GETEVENT over the whole range, one or more events: what window_owner.cpp asks for.
int SDL_PeepEvents(SDL_Event* events, int numevents, SDL_EventAction action, Uint32 minType, Uint32 maxType) {
    if (action != SDL_GETEVENT || minType != SDL_EVENT_FIRST || maxType != SDL_EVENT_LAST) {
        SDL_SetError("SDL_PeepEvents: only SDL_GETEVENT over every event type is supported here");
        return -1;
    }
    int n = 0;
    while (n < numevents && SDL_PollEvent(events != NULL ? &events[n] : NULL))
        ++n;
    return n;
}
bool SDL_WaitEventTimeout(SDL_Event* event, Sint32 timeoutMS) {
    const Uint64 end = SDL_GetTicks() + (timeoutMS < 0 ? 0 : (Uint64)timeoutMS);
    for (;;) {
        if (SDL_PollEvent(event))
            return true;
        if (timeoutMS >= 0 && SDL_GetTicks() >= end)
            return false;
        svcSleepThread(1000000);  // 1 ms
    }
}
