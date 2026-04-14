#ifndef FRONT_WINDOW_H
#define FRONT_WINDOW_H

#include <SDL2/SDL.h>
#include <imgui.h>

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


#endif
