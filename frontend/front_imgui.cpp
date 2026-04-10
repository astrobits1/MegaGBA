#include <SDL2/SDL.h>
#include <gba/gba.h>

#include <iostream>
#include <vector>

typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
} Context;


/* ------------------------------------------------------------------- */

static bool initialiseSDL(SDL_Window** window, SDL_Renderer** renderer, SDL_Texture** texture) {
    SDL_Init(SDL_INIT_EVERYTHING);

    SDL_Window* win = SDL_CreateWindow("MegaGBA", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIDTH_PX * DISPLAY_SCALING, HEIGHT_PX * DISPLAY_SCALING, SDL_WINDOW_SHOWN);

    if (win == NULL) return false;          /* Failed to create screen */

    SDL_Renderer* rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);

    if (rend == NULL) return false; 

    SDL_Texture* text = SDL_CreateTexture(rend, SDL_PIXELFORMAT_BGR555, SDL_TEXTUREACCESS_STREAMING, WIDTH_PX, HEIGHT_PX);

    if (text == NULL) return false;

    *window = win;
    *renderer = rend;
    *texture = text;

    SDL_RenderSetScale(rend, DISPLAY_SCALING, DISPLAY_SCALING);
    SDL_RenderClear(rend);
    return true;
}

static void SDLEvents(GBA* gba) {
    /* We listen for events like keystrokes and window closing */
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            gba->run = false;
        } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
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

static void cleanSDL(SDL_Window* win, SDL_Renderer* rend, SDL_Texture* text) {
    SDL_DestroyTexture(text);
	SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    SDL_Quit();
}

void frameEndCallback(GBA* gba, void* context) {
    Context* ctx = (Context*)context;

    SDLEvents(gba);

    /* Update texture with framebuffer and render it at the end of frame */
    SDL_UpdateTexture(ctx->texture, NULL, &gba->framebuffer, WIDTH_PX*sizeof(uint16_t));

    SDL_RenderClear(ctx->renderer);
    SDL_RenderCopy(ctx->renderer, ctx->texture, NULL, NULL);
	SDL_RenderPresent(ctx->renderer);
}


 int frontendImguiMain(std::vector<uint8_t>& buffer, size_t size, std::vector<uint8_t>& biosBuffer, size_t biosSize) {
    uint8_t* cbuffer = reinterpret_cast<uint8_t*>(buffer.data());
    uint8_t* cbiosBuffer = reinterpret_cast<uint8_t*>(biosBuffer.data());

    GamePak gamepak;
    initGamePak(&gamepak, cbuffer, size); 

    Context ctx;
    bool doneInitSDL = initialiseSDL(&ctx.window, &ctx.renderer, &ctx.texture);
    if (!doneInitSDL) {
        std::cout << "Could not initialise SDL\n";
        return 8;
    }

    /* Mainloop */
    GBA gba;
    initialiseGBA(&gba, &gamepak, cbiosBuffer, biosSize, frameEndCallback, &ctx);
    startGBAEmulator(&gba);
    freeGBA(&gba);
    cleanSDL(ctx.window, ctx.renderer, ctx.texture);
    return 0;
}
