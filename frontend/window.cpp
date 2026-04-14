#include <frontend/window.hpp>
#include <frontend/context.hpp>

#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_sdlrenderer2.h>

#include <algorithm>

/* ---------------- SDL ---------------- */

static bool initialiseSDL(SDL_Window*& window, SDL_Renderer*& renderer) {
    SDL_Window* win = SDL_CreateWindow("MegaGBA", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 0, 0, SDL_WINDOW_SHOWN);

    if (win == NULL) return false;          /* Failed to create screen */

    SDL_Renderer* rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);

    if (rend == NULL) return false;  

    window = win;
    renderer = rend;

    SDL_RenderClear(rend);
    return true;
}

static void cleanSDL(SDL_Window* win, SDL_Renderer* rend) {
	SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
}

/* ------------------------------------- */

Window::Window(Context* ctx) {
    this->context = ctx;
}

void Window::initialise() {
    ImGuiContext* prevContext = ImGui::GetCurrentContext();

    /* Initialise SDL window and renderer */
    bool doneInit = initialiseSDL(this->window, this->renderer);
    if (!doneInit) throw;

    /* Add SDL window to context SDL windows list */
    this->context->windows.push_back(this);

    /* Initialise ImGUI context for window */
    ImGuiContext* imguiCtx = ImGui::CreateContext();
    /* Created context auto set to current context */
    this->imguiCtx = imguiCtx;
    ImGui::SetCurrentContext(imguiCtx);

	ImGui_ImplSDLRenderer2_Init(this->renderer);
    ImGui_ImplSDL2_InitForSDLRenderer(this->window, this->renderer);

    this->initialised = true;
    this->showing = true;

    if (prevContext != NULL) ImGui::SetCurrentContext(prevContext);
}

void Window::hide() {
    if (!this->initialised) return;

    this->showing = false;
    SDL_HideWindow(this->window);
}

void Window::show() {
    if (!this->initialised) return;

    this->showing = true;
    SDL_ShowWindow(this->window);
}

Window::~Window() {
    if (this->initialised) {
        ImGuiContext* prevContext = ImGui::GetCurrentContext();

        /* Destroy main window ImGUI context and then SDL components */
        ImGui::SetCurrentContext(this->imguiCtx);
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();

        ImGui::SetCurrentContext(this->imguiCtx);
        ImGui::DestroyContext();

        /* Manage SDL Window list for context */
        std::vector<Window*>* vec = &this->context->windows;
        vec->erase(std::remove(vec->begin(), vec->end(), (Window*)this), vec->end());

        cleanSDL(this->window, this->renderer);

        if (this->imguiCtx != prevContext) {
            ImGui::SetCurrentContext(prevContext);
        }
    }
}

/* ------------------- Main Window -------------------- */

void MainWindow::initialise() {
    Window::initialise();

    int w = WIDTH_PX * DISPLAY_SCALING;
    int h = (HEIGHT_PX + MENU_HEIGHT_PX) * DISPLAY_SCALING;

    SDL_Rect displayBounds;
    SDL_GetDisplayBounds(0, &displayBounds);
    int scx = displayBounds.x + (displayBounds.w / 2);
    int scy = displayBounds.y + (displayBounds.h / 2);

    SDL_SetWindowSize(this->window, w, h);
    SDL_SetWindowPosition(this->window, 0, 0);

    SDL_SetWindowTitle(this->window, "MegaGBA");
    SDL_SetWindowAlwaysOnTop(this->window, (SDL_bool)true);
    SDL_RenderSetScale(this->renderer, DISPLAY_SCALING, DISPLAY_SCALING);
}

/* ------------------- Debugger Window ---------------- */

void DisassemblerWindow::initialise() {
    Window::initialise();

    int w = 650;
    int h = 800;

    int mx, my, mw, mh;
    SDL_GetWindowSize(this->context->mainWin.window, &mw, &mh);
    SDL_GetWindowPosition(this->context->mainWin.window, &mx, &my);

    SDL_SetWindowPosition(this->window, 0, 0);
    SDL_SetWindowSize(this->window, w, h);

    SDL_SetWindowTitle(this->window, "Disassembler");
}

void DisassemblerWindow::show() {
    Window::show();
}

