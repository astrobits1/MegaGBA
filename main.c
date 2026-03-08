#include <SDL2/SDL.h>
#include <gba/gba.h>
#include <gba/gamepak.h>
#include <stdio.h>

int main(int argc, char** argv) {
    if (argc == 1) {
        printf("Welcome to MegaGBA!\nUsage: megagba [FILEPATH]\n");
        return 0;
    }

    FILE* file;
    size_t size;

    file = fopen(argv[1], "r");
    if (file == NULL) {
        printf("Error opening file: %s\nPlease check your file path\n", argv[1]);
        return 1;
    }

    fseek(file, 0L, SEEK_END);
    size = ftell(file);
    rewind(file);

    uint8_t* buffer = malloc(size);
    if (buffer == NULL) {
        printf("An error occured with buffer allocation\n");
        fclose(file);
        return 2;
    }

    size_t readCount = fread(buffer, 1, size, file);
    if (readCount != size) {
        printf("An error occured while reading ROM\n");
        fclose(file);
        return 3;
    }

    fclose(file);

    /* At this stage, file reading and copying to memory is complete */
    
    GamePak gamepak;
    initGamePak(&gamepak, buffer, size);

    GBA* gba = malloc(sizeof(GBA));
    if (gba == NULL) {
        printf("An error occured while allocating emulator space\n");
        freeGamePak(&gamepak);
        return 4;
    }

    initialiseGBA(gba, &gamepak);
    /* Mainloop */
    startGBAEmulator(&gamepak);
    /* --------------- */
    freeGBA(gba);
    freeGamePak(&gamepak);
    return 0;
}
