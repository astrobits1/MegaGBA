#include <SDL2/SDL.h>
#include <gba/gba.h>
#include <imgui.h> 
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_sdlrenderer2.h>

#include <frontend/context.hpp>

#include <iostream>
#include <vector>
#include <unistd.h>
#include <sys/select.h>


/* ------------------------------------------------------------------- */

static void ImGuiEvents(Context* ctx, SDL_Event* event) {
    if (event->type == SDL_WINDOWEVENT) {
        if (event->window.event == SDL_WINDOWEVENT_CLOSE) {
            if (event->window.windowID == SDL_GetWindowID(ctx->mainWin.window)) {
                /* If main window is closed, close whole program */
                ctx->quit = true;
            } else {
                /* Scan through all child and descendent windows of context and trigger close
                 * window for it if window ID matches */
                for (int i=0; i<ctx->windows.size(); i++) {
                    Window* win = ctx->windows.at(i);
                    if (event->window.windowID == SDL_GetWindowID(win->window)) {
                        win->hide();
                        break;
                    }
                }
            }
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            /* Scan through all child and descendent windows of context and set it to currenet
             * context if window ID matches */
            for (int i=0; i<ctx->windows.size(); i++) {
                Window* win = ctx->windows.at(i);
                if (event->window.windowID == SDL_GetWindowID(win->window)) {
                    ImGui::SetCurrentContext(win->imguiCtx);
                    break;
                }
            }
        }
    }


    /* Events are processed for the current ImGui context aka window only */
    ImGui_ImplSDL2_ProcessEvent(event);
}

static void SDLEvents(Context* ctx) {
    GBA* gba = ctx->gba;
    /* We listen for events like keystrokes and window closing */
    SDL_Event event = {};

    while (SDL_PollEvent(&event)) {
        ImGuiEvents(ctx, &event);

        if (event.type == SDL_QUIT) {
            ctx->quit = true;
        } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0 && SDL_GetKeyboardFocus() == ctx->mainWin.window) {
            /* Handle keydown by updating KEYINPUT */
            switch (event.key.keysym.scancode) {
                case SDL_SCANCODE_DOWN: keyinputReset(gba, KEYINPUT_DPDOWN); break;
                case SDL_SCANCODE_UP: keyinputReset(gba, KEYINPUT_DPUP); break;
                case SDL_SCANCODE_LEFT: keyinputReset(gba, KEYINPUT_DPLEFT); break;
                case SDL_SCANCODE_RIGHT: keyinputReset(gba, KEYINPUT_DPRIGHT); break;

                case SDL_SCANCODE_Z: keyinputReset(gba, KEYINPUT_A); break;
                case SDL_SCANCODE_X: keyinputReset(gba, KEYINPUT_B); break;
                case SDL_SCANCODE_RETURN: keyinputReset(gba, KEYINPUT_START); break;
                case SDL_SCANCODE_TAB: keyinputReset(gba, KEYINPUT_SELECT); break;
                case SDL_SCANCODE_A: keyinputReset(gba, KEYINPUT_L); break;
                case SDL_SCANCODE_S: keyinputReset(gba, KEYINPUT_R); break;

                default: break;
            }

        } else if (event.type == SDL_KEYUP && event.key.repeat == 0) {
            /* Handle keyup by updating KEYINPUT */
            switch (event.key.keysym.scancode) {
                case SDL_SCANCODE_DOWN: keyinputSet(gba, KEYINPUT_DPDOWN); break;
                case SDL_SCANCODE_UP: keyinputSet(gba, KEYINPUT_DPUP); break;
                case SDL_SCANCODE_LEFT: keyinputSet(gba, KEYINPUT_DPLEFT); break;
                case SDL_SCANCODE_RIGHT: keyinputSet(gba, KEYINPUT_DPRIGHT); break;

                case SDL_SCANCODE_Z: keyinputSet(gba, KEYINPUT_A); break;
                case SDL_SCANCODE_X: keyinputSet(gba, KEYINPUT_B); break;
                case SDL_SCANCODE_RETURN: keyinputSet(gba, KEYINPUT_START); break;
                case SDL_SCANCODE_TAB: keyinputSet(gba, KEYINPUT_SELECT); break;
                case SDL_SCANCODE_A: keyinputSet(gba, KEYINPUT_L); break;
                case SDL_SCANCODE_S: keyinputSet(gba, KEYINPUT_R); break; 

                default: break;
            }
        }
    }
}

/* ----------------- GUI Rendering -------------------- */


static void renderMainGUI(Context* ctx) {
    PUSH_IMGUI_CTX();

    ImGui::StyleColorsDark();
    ImGui::SetCurrentContext(ctx->mainWin.imguiCtx);
    ImGui_ImplSDLRenderer2_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2(240*DISPLAY_SCALING, MENU_HEIGHT_PX*DISPLAY_SCALING));
	ImGui::SetNextWindowPos(ImVec2(0, 0));

	ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;

    ImGui::Begin("MainMenu", NULL, windowFlags);
    ImGui::SetWindowFontScale(1.8);
    if (ImGui::BeginMenuBar()) {

		if (ImGui::BeginMenu("File")) {
            ImGui::SetWindowFontScale(1.8);
			if (ImGui::MenuItem("Open ROM (.gba)")) {}
			ImGui::EndMenu();
		}

        if (ImGui::BeginMenu("Tools")) {
            ImGui::SetWindowFontScale(1.8);
			if (ImGui::MenuItem("Disassembler", NULL, &ctx->mainWin.disassemblerWin.showing)) {
                if (!ctx->mainWin.disassemblerWin.initialised) ctx->mainWin.disassemblerWin.initialise();

                if (ctx->mainWin.disassemblerWin.showing) ctx->mainWin.disassemblerWin.show();
                else ctx->mainWin.disassemblerWin.hide();
            }
			ImGui::EndMenu();
		}


		ImGui::EndMenuBar();
	}

    ImGui::End();
    ImGui::Render();

    SDL_RenderSetScale(ctx->mainWin.renderer, 1, 1);
	ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ctx->mainWin.renderer);

    POP_IMGUI_CTX();
}

static void renderDisassemblerGUI(Context* ctx) {
    PUSH_IMGUI_CTX();

    ImGui::SetCurrentContext(ctx->mainWin.disassemblerWin.imguiCtx);
    ImGuiIO io = ImGui::GetIO();

    int w, h;
    SDL_GetWindowSize(ctx->mainWin.disassemblerWin.window, &w, &h);
#define REL_X(r) (r*(w/io.DisplayFramebufferScale.x))    // r=0->1
#define REL_Y(r) (r*(h/io.DisplayFramebufferScale.y))   // r=0->1

    ImGui_ImplSDLRenderer2_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame(); 

    ImGui::Begin("Hello");
    ImGui::SetWindowSize(ImVec2(REL_X(1), REL_Y(1)));
	ImGui::SetWindowPos(ImVec2(0, 0));


    ImGui::End();

    ImGui::Render();

	ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ctx->mainWin.disassemblerWin.renderer);

    POP_IMGUI_CTX();
}


/* ---------------- Context --------------------------- */
Context::Context() : mainWin(this), console(this), quit(false), gba(NULL) {
    SDL_Init(SDL_INIT_EVERYTHING);
    this->mainWin.initialise();

    /* Initialise texture for main window */
    SDL_Texture* text = SDL_CreateTexture(this->mainWin.renderer, SDL_PIXELFORMAT_BGR555, SDL_TEXTUREACCESS_STREAMING, 240, 160);
    if (text == NULL) throw;
    this->texture = text; 

}

Context::~Context() {
    if (this->gba != NULL) {
        unloadROM();
    }  

    SDL_DestroyTexture(this->texture);
    SDL_Quit();
}

void Context::loadROM(std::vector<uint8_t>& buffer, size_t size, std::vector<uint8_t>& biosBuffer, size_t biosSize) {
    /* File and ROM buffers are left untouched and used from reference */
    if (this->gba != NULL) {
        freeGBA(this->gba);
        delete this->gba;
    }

    uint8_t* cbuffer = reinterpret_cast<uint8_t*>(buffer.data());
    uint8_t* cbiosBuffer = reinterpret_cast<uint8_t*>(biosBuffer.data());

    this->gba = new GBA;

    GamePak gamepak;
    initGamePak(&gamepak, cbuffer, size);
    initialiseGBA(this->gba, gamepak, cbiosBuffer, biosSize);
}

void Context::unloadROM() {
    /* File and ROM buffers are completely left untouched */
    freeGamePak(&this->gba->gamepak);
    freeGBA(this->gba);
    delete this->gba;

    this->gba = NULL;
}

/* ---------------------------------------------------------- */

/* Check if input is available in STDIN */
bool inputAvailable() {
  struct timeval tv;
  fd_set fds;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  select(STDIN_FILENO+1, &fds, NULL, NULL, &tv);
  return (FD_ISSET(0, &fds));
}

void runFrame(Context* ctx) {
    /* Step frame on emulator */
    stepGBAFrame(ctx->gba);

    /* Check for on terminal console inputs */
    if (inputAvailable()) {
        std::string input;
        std::cin >> input;
        std::string output = ctx->console.run(input);
        std::cout << output;
    }

    /* Render framebuffer to SDL renderer after setting display scale */
    /* Update texture with framebuffer and render it at the end of frame */
   
    SDL_UpdateTexture(ctx->texture, NULL, &ctx->gba->framebuffer, 240*sizeof(uint16_t));
    SDL_RenderSetScale(ctx->mainWin.renderer, DISPLAY_SCALING, DISPLAY_SCALING);
    SDL_RenderClear(ctx->mainWin.renderer);

    SDLEvents(ctx); 

    SDL_Rect dstrect;
    dstrect.h = 160;
    dstrect.w = 240;
    dstrect.x = 0;
    dstrect.y = MENU_HEIGHT_PX;

    SDL_RenderCopy(ctx->mainWin.renderer, ctx->texture, NULL, &dstrect);

    /* Render IMGUI to the same renderer as an overlap */
    renderMainGUI(ctx);
    SDL_RenderPresent(ctx->mainWin.renderer);

    /* Render sub windows */
    if (ctx->mainWin.disassemblerWin.showing) {
        renderDisassemblerGUI(ctx);
        SDL_RenderPresent(ctx->mainWin.disassemblerWin.renderer);
    }
}


 int frontendImguiMain(std::vector<uint8_t> buffer, size_t size, std::vector<uint8_t> biosBuffer, size_t biosSize) {
    try {
        Context ctx;
        ctx.loadROM(buffer, size, biosBuffer, biosSize); 

        /* Mainloop */
        while (!ctx.quit) {
            runFrame(&ctx); 
        }
    
        ctx.unloadROM();
    } catch (int) {
        std::cout << "Could not initialise SDL\n";
        return 8;
    }

    return 0;
}
