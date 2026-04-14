#ifndef FRONT_IMGUI_H
#define FRONT_IMGUI_H

#include <SDL2/SDL.h>
#include <gba/gba.h>
#include <frontend/console.hpp>
#include <frontend/window.hpp>
#include <imgui.h>
#include <vector>
#include <stdint.h>

#define DISPLAY_SCALING 4
#define MENU_HEIGHT_PX 7

#define PUSH_IMGUI_CTX() ImGuiContext* prevImguiCtx = ImGui::GetCurrentContext()
#define POP_IMGUI_CTX() ImGui::SetCurrentContext(prevImguiCtx)

class Context;

class Context {
public:
    bool quit = false;

    /* SDL State */
    SDL_Texture* texture;

    /* Console state */
    Console console;

    /* GUI State */
    std::vector<Window*> windows;
    MainWindow mainWin;

    /* Emulator state */
    GBA* gba;

    /* Methods */
    void loadROM(std::vector<uint8_t>& buffer, size_t size, std::vector<uint8_t>& biosBuffer, size_t biosSize);
    void unloadROM();
    Context();
    ~Context();
};



int frontendImguiMain(std::vector<uint8_t> buffer, size_t size, std::vector<uint8_t> biosBuffer, size_t biosSize);

#endif
