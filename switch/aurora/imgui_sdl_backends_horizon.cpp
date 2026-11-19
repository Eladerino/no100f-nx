// ImGui's SDL 3 platform and SDL renderer backends, as the Switch has them: none. The SDL here is
// the small libnx shim (switch/sdl3_shim), which has no windows, cursors, clipboard or SDL_Renderer,
// so imgui_impl_sdl3.cpp and imgui_impl_sdlrenderer3.cpp are taken out of Aurora's imgui_backends and
// these take their place (SpmSwitch.cmake). ImGui draws through imgui_impl_wgpu. Aurora's
// imgui::new_frame sets the display size; the one thing the platform backend's NewFrame still owes
// ImGui is the frame time. Same approach as SwitchWaker's aurora/patches/0002, without patching
// Aurora's own source.
//
// ProcessEvent below is real, though: the only key events in the shim's SDL queue are the ones the
// gamepad shim synthesises (the F10 menu tap), and those are mapped into ImGui key events.
// Gamepad input takes the other path: NewFrame below polls the shim's one gamepad and feeds
// ImGui's gamepad keys directly, because the settings menu navigates on those (its page keys
// read ImGuiKey_GamepadL1/R1) and no SDL event ever carries them.
#include <chrono>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

bool ImGui_ImplSDL3_InitForSDLRenderer(SDL_Window*, SDL_Renderer*) { return true; }
void ImGui_ImplSDL3_Shutdown() {}

// The navigation set, plus Space and the F-keys so F10/F11 stay visible to
// ImGui; anything else the queue might carry is not menu input.
static ImGuiKey ImGuiKeyFromScancode(SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
    case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    default: break;
    }
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (scancode - SDL_SCANCODE_F1));
    return ImGuiKey_None;
}

bool ImGui_ImplSDL3_ProcessEvent(const SDL_Event* event) {
    if (ImGui::GetCurrentContext() == nullptr)
        return false;
    ImGuiIO& io = ImGui::GetIO();
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const ImGuiKey key = ImGuiKeyFromScancode(event->key.scancode);
        if (key == ImGuiKey_None)
            return false;
        const bool down = event->type == SDL_EVENT_KEY_DOWN;
        // Backends report modifier state through the Mod keys as well as the
        // physical ones; Shift+Tab only reads as "previous" with both set.
        if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift)
            io.AddKeyEvent(ImGuiMod_Shift, down);
        if (key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt)
            io.AddKeyEvent(ImGuiMod_Alt, down);
        io.AddKeyEvent(key, down);
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        io.AddInputCharactersUTF8(event->text.text);
        return true;
    default:
        return false;
    }
}

void ImGui_ImplSDL3_NewFrame() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float delta = std::chrono::duration<float>(now - last).count();
    last = now;
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = delta > 0.0f ? delta : 1.0f / 60.0f;

    // Feed the pad as ImGui gamepad keys, the way imgui_impl_sdl3.cpp's
    // UpdateGamepads does: the shim's getters read the pump's latest sample,
    // so this is the current state of the one pad, published every frame.
    static SDL_Gamepad* gamepad = nullptr;
    if (gamepad == nullptr) {
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            if (count > 0)
                gamepad = SDL_OpenGamepad(ids[0]);
            SDL_free(ids);
        }
    }
    if (gamepad == nullptr) {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        return;
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const auto button = [&io](ImGuiKey key, SDL_GamepadButton which) {
        io.AddKeyEvent(key, SDL_GetGamepadButton(gamepad, which));
    };
    const auto analog = [&io](ImGuiKey key, SDL_GamepadAxis axis, float v0, float v1) {
        float value = (static_cast<float>(SDL_GetGamepadAxis(gamepad, axis)) - v0) / (v1 - v0);
        value = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
        io.AddKeyAnalogEvent(key, value > 0.1f, value);
    };
    button(ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START);
    button(ImGuiKey_GamepadBack, SDL_GAMEPAD_BUTTON_BACK);
    button(ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST);
    button(ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST);
    button(ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH);
    button(ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH);
    button(ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    button(ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    button(ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP);
    button(ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    button(ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    button(ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    analog(ImGuiKey_GamepadL2, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.0f, 32767.0f);
    analog(ImGuiKey_GamepadR2, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.0f, 32767.0f);
    button(ImGuiKey_GamepadL3, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    button(ImGuiKey_GamepadR3, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    constexpr float kThumbDeadZone = 8000.0f;
    analog(ImGuiKey_GamepadLStickLeft, SDL_GAMEPAD_AXIS_LEFTX, -kThumbDeadZone, -32768.0f);
    analog(ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX, kThumbDeadZone, 32767.0f);
    analog(ImGuiKey_GamepadLStickUp, SDL_GAMEPAD_AXIS_LEFTY, -kThumbDeadZone, -32768.0f);
    analog(ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY, kThumbDeadZone, 32767.0f);
    analog(ImGuiKey_GamepadRStickLeft, SDL_GAMEPAD_AXIS_RIGHTX, -kThumbDeadZone, -32768.0f);
    analog(ImGuiKey_GamepadRStickRight, SDL_GAMEPAD_AXIS_RIGHTX, kThumbDeadZone, 32767.0f);
    analog(ImGuiKey_GamepadRStickUp, SDL_GAMEPAD_AXIS_RIGHTY, -kThumbDeadZone, -32768.0f);
    analog(ImGuiKey_GamepadRStickDown, SDL_GAMEPAD_AXIS_RIGHTY, kThumbDeadZone, 32767.0f);
}

// Aurora only takes the SDL renderer path when the window has an SDL_Renderer, which never happens
// here (the shim's window has none).
// FrightWorks' Aurora asks for every connected pad (lib/imgui.cpp). The Switch shim has one pad
// slot and ImGui_ImplSDL3_NewFrame above reads it, so there is nothing to choose.
void ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode, SDL_Gamepad**, int) {}

bool ImGui_ImplSDLRenderer3_Init(SDL_Renderer*) { return false; }
void ImGui_ImplSDLRenderer3_Shutdown() {}
void ImGui_ImplSDLRenderer3_NewFrame() {}
void ImGui_ImplSDLRenderer3_RenderDrawData(ImDrawData*, SDL_Renderer*) {}
