#ifndef FRONT_IMGUI_H
#define FRONT_IMGUI_H

#include <SDL2/SDL.h>
#include <gba/gba.h>
#include <imgui.h>
#include <vector>
#include <stdint.h>

#define DISPLAY_SCALING 4
#define MENU_HEIGHT_PX 7

#define PUSH_IMGUI_CTX() ImGuiContext* prevImguiCtx = ImGui::GetCurrentContext()
#define POP_IMGUI_CTX() ImGui::SetCurrentContext(prevImguiCtx)

class Context;

class Window {
public:
    SDL_Window* window = NULL;
    SDL_Renderer* renderer = NULL;

    Context* context = NULL;
    ImGuiContext* imguiCtx = NULL;

    bool showing = false;
    bool initialised = false;

    void initialise();
    void show();
    void hide();
    Window(Context* ctx);
    ~Window();
};

class DisassemblerWindow : public Window {
public:
    void initialise();
    void show();

    DisassemblerWindow(Context* c) : Window(c) {}
};

class MainWindow : public Window {
public:
    DisassemblerWindow disassemblerWin;

    void initialise();
    MainWindow(Context* c) : Window(c), disassemblerWin(c) {};
};

class Context {
public:
    bool quit = false;

    /* SDL State */
    SDL_Texture* texture;

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
