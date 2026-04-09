#include <SDL2/SDL.h>
#include <gba/gba.h>
#include <gba/gamepak.h>
#include <stdio.h>

typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
} Context;

/* ------------------------------------------------------------------- */

bool initialiseSDL(SDL_Window** window, SDL_Renderer** renderer, SDL_Texture** texture) {
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

void SDLEvents(GBA* gba) {
    /* We listen for events like keystrokes and window closing */
    SDL_Event event;

#define KEYINPUT_SET(b)     writeIO_internal(gba, KEYINPUT, readIO_internal(gba, KEYINPUT, WIDTH_16) | (1 << b), WIDTH_16)
#define KEYINPUT_RESET(b)   writeIO_internal(gba, KEYINPUT, readIO_internal(gba, KEYINPUT, WIDTH_16) & ~(1 << b), WIDTH_16)

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            gba->run = false;
        } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
            /* Handle keydown by updating KEYINPUT */
            switch (event.key.keysym.scancode) {
                case SDL_SCANCODE_DOWN: KEYINPUT_RESET(7); break;
                case SDL_SCANCODE_UP: KEYINPUT_RESET(6); break;
                case SDL_SCANCODE_LEFT: KEYINPUT_RESET(5); break;
                case SDL_SCANCODE_RIGHT: KEYINPUT_RESET(4); break;

                case SDL_SCANCODE_Z: KEYINPUT_RESET(0); break;
                case SDL_SCANCODE_X: KEYINPUT_RESET(1); break;
                case SDL_SCANCODE_RETURN: KEYINPUT_RESET(3); break;
                case SDL_SCANCODE_TAB: KEYINPUT_RESET(2); break;
                case SDL_SCANCODE_A: KEYINPUT_RESET(9); break;
                case SDL_SCANCODE_S: KEYINPUT_RESET(8); break;

                default: break;
            }

        } else if (event.type == SDL_KEYUP && event.key.repeat == 0) {
            /* Handle keyup by updating KEYINPUT */
            switch (event.key.keysym.scancode) {
                case SDL_SCANCODE_DOWN: KEYINPUT_SET(7); break;
                case SDL_SCANCODE_UP: KEYINPUT_SET(6); break;
                case SDL_SCANCODE_LEFT: KEYINPUT_SET(5); break;
                case SDL_SCANCODE_RIGHT: KEYINPUT_SET(4); break;

                case SDL_SCANCODE_Z: KEYINPUT_SET(0); break;
                case SDL_SCANCODE_X: KEYINPUT_SET(1); break;
                case SDL_SCANCODE_RETURN: KEYINPUT_SET(3); break;
                case SDL_SCANCODE_TAB: KEYINPUT_SET(2); break;
                case SDL_SCANCODE_A: KEYINPUT_SET(9); break;
                case SDL_SCANCODE_S: KEYINPUT_SET(8); break;

                default: break;
            }
        }
    }
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

void cleanSDL(SDL_Window* win, SDL_Renderer* rend, SDL_Texture* text) {
    SDL_DestroyTexture(text);
	SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    SDL_Quit();
}


static int readBin(char* filePath, size_t* _size, uint8_t** _buffer, size_t maxSize) {
    FILE* file;
    size_t size;

    file = fopen(filePath, "r");
    if (file == NULL) {
        printf("Error opening file: %s\nPlease check your file path\n", filePath);
        return 1;
    }

    fseek(file, 0L, SEEK_END);
    size = ftell(file);
    rewind(file);

    /* Truncate it */
    if (size > maxSize) size = maxSize;

    uint8_t* buffer = malloc(size);
    if (buffer == NULL) {
        printf("An error occured with buffer allocation\n");
        fclose(file);
        return 2;
    }

    size_t readCount = fread(buffer, 1, size, file);
    if (readCount != size) {
        printf("An error occured while reading file\n");
        fclose(file);
        free(buffer);
        return 3;
    }

    fclose(file);
    *_buffer = buffer;
    *_size = size;

    return 0;
}

int main(int argc, char** argv) {
    uint8_t* biosBuffer = NULL;
    size_t biosSize = 0;

    /* Check for flags and check file type */
    char* filepath = NULL;

    for (int i=1; i<argc; i++) {
        char* flag = argv[i];
        if (strcmp(flag, "-bios") == 0) {
            if (argc < 4) {
                printf("Please specify filepath for '-bios'\n");
                return 4;
            }
            if (biosBuffer != NULL) {
                printf("BIOS file already specified\n");
                free(biosBuffer);
                return 5;
            }
            /* Load BIOS file */
            int err = readBin(argv[i+1], &biosSize, &biosBuffer, 0x4000);
            if (err != 0) {
                printf("Could not load BIOS file: %s\n", argv[i+1]);
                return err;
            }
                
            /* Filepath has been read */
            i++;
        } else if (strcmp(flag, "-help") == 0) {
            /* Ignore help if other instructions are specified */
            if (argc != 2) continue;

            printf("Welcome to MegaGBA!\n");
            printf("Usage:\n");
            printf("   megagba [ROM FILEPATH]    (Specifies .gba ROM file)\n\n");
            printf("Add-Ons:\n");
            printf("   -bios [BIOS FILEPATH]    (Specifies bios ROM file)\n");

            printf("\n");
            printf("Help:\n");
            printf("   megagba -help\n");
            return 0;
        } else {
            if (filepath != NULL) {
                printf("Invalid argument sequence\n");
                return 6;
            }
            filepath = flag;
        }
    }

    if (filepath == NULL) {
        printf("Please specify filepath\n");
        printf("\nRun:\n   megagba -help\nFor usage information\n");
        return 7;
    }

    uint8_t* buffer;
    size_t size;
    int err = readBin(filepath, &size, &buffer, 0x02000000);
    if (err != 0) {
        printf("Could not load ROM file\n");
        return err;
    }

    /* At this stage, file reading and copying to memory is complete */
    
    GamePak gamepak;
    initGamePak(&gamepak, buffer, size); 

    Context ctx;
    bool doneInitSDL = initialiseSDL(&ctx.window, &ctx.renderer, &ctx.texture);
    if (!doneInitSDL) {
        printf("Could not initialise SDL\n");
        return 8;
    }

    /* Mainloop */
    GBA gba;
    initialiseGBA(&gba, &gamepak, biosBuffer, biosSize, frameEndCallback, &ctx);
    startGBAEmulator(&gba);
    freeGBA(&gba);
    /* --------------- */
    free(biosBuffer);
    freeGamePak(&gamepak);
    return 0;
}
